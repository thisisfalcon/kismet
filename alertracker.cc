/*
   This file is part of Kismet

   Kismet is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   Kismet is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Kismet; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "config.hpp"

#include <string>
#include <vector>
#include <sstream>

#include "alertracker.h"
#include "devicetracker.h"
#include "configfile.h"

#include "json_adapter.h"
#include "msgpack_adapter.h"

Alertracker::Alertracker(GlobalRegistry *in_globalreg) :
    Kis_Net_Httpd_CPPStream_Handler(in_globalreg) {
	globalreg = in_globalreg;
	next_alert_id = 0;

    pthread_mutex_init(&alert_mutex, NULL);

	if (globalreg->kismet_config == NULL) {
		fprintf(stderr, "FATAL OOPS:  Alertracker called with null config\n");
		exit(1);
	}

    packetchain =
        static_pointer_cast<Packetchain>(globalreg->FetchGlobal("PACKETCHAIN"));
    entrytracker =
        static_pointer_cast<EntryTracker>(globalreg->FetchGlobal("ENTRY_TRACKER"));

	if (globalreg->kismet_config->FetchOpt("alertbacklog") != "") {
		int scantmp;
		if (sscanf(globalreg->kismet_config->FetchOpt("alertbacklog").c_str(), 
				   "%d", &scantmp) != 1 || scantmp < 0) {
			globalreg->messagebus->InjectMessage("Illegal value for 'alertbacklog' "
												 "in config file", MSGFLAG_FATAL);
			globalreg->fatal_condition = 1;
			return;
		}
		num_backlog = scantmp;
	}

	// Parse config file vector of all alerts
	if (ParseAlertConfig(globalreg->kismet_config) < 0) {
		_MSG("Failed to parse alert values from Kismet config file", MSGFLAG_FATAL);
		globalreg->fatal_condition = 1;
		return;
	}

    alert_vec_id =
        entrytracker->RegisterField("kismet.alert.list",
                TrackerVector, "list of alerts");
    alert_timestamp_id =
        entrytracker->RegisterField("kismet.alert.timestamp",
                TrackerUInt64, "alert update timestamp");

    shared_ptr<tracked_alert> alert_builder(new tracked_alert(globalreg, 0));
    alert_entry_id =
        entrytracker->RegisterField("kismet.alert.alert",
                alert_builder, "Kismet alert");

    alert_defs = 
        entrytracker->RegisterAndGetField("kismet.alert.definition_list",
                TrackerVector, "Kismet alert definitions");
    alert_defs_vec = TrackerElementVector(alert_defs);

    shared_alert_def def_builder(new tracked_alert_definition(globalreg, 0));
    alert_def_id =
        entrytracker->RegisterField("kismet.alert.alert_definition",
                def_builder, "Kismet alert definition");

	// Register the alert component
	_PCM(PACK_COMP_ALERT) =
		packetchain->RegisterPacketComponent("alert");

	// Register a KISMET alert type with no rate restrictions
	_ARM(ALERT_REF_KISMET) =
		RegisterAlert("KISMET", "Server events", sat_day, 0, sat_day, 0, KIS_PHY_ANY);

	
	_MSG("Created alert tracker...", MSGFLAG_INFO);
}

Alertracker::~Alertracker() {
    pthread_mutex_lock(&alert_mutex);

    globalreg->RemoveGlobal("ALERTTRACKER");
    globalreg->alertracker = NULL;

    pthread_mutex_destroy(&alert_mutex);
}

int Alertracker::RegisterAlert(string in_header, string in_description, 
        alert_time_unit in_unit, int in_rate, alert_time_unit in_burstunit,
        int in_burst, int in_phy) {
    local_locker lock(&alert_mutex);

	// Bail if this header is registered
	if (alert_name_map.find(in_header) != alert_name_map.end()) {
        _MSG("Tried to re-register duplicate alert " + in_header, MSGFLAG_ERROR);
		return -1;
	}

    // Make sure we're not going to overstep our range
    if ((unsigned int) in_burstunit > sat_day)
        in_burstunit = sat_day;
    if ((unsigned int) in_unit > sat_day)
        in_unit = sat_day;

    // Bail if the rates are impossible
    if (in_burstunit > in_unit) {
        _MSG("Failed to register alert " + in_header + ", time unit for "
                "burst rate must be less than or equal to the time unit "
                "for the max rate", MSGFLAG_ERROR);
        return -1;
    }

    shared_alert_def arec = static_pointer_cast<tracked_alert_definition>(entrytracker->GetTrackedInstance(alert_def_id));

    arec->set_alert_ref(next_alert_id++);
    arec->set_header(StrUpper(in_header));
    arec->set_description(in_description);
    arec->set_limit_unit(in_unit);
    arec->set_limit_rate(in_rate);
    arec->set_burst_unit(in_burstunit);
    arec->set_limit_burst(in_burst);
    arec->set_phy(in_phy);
    arec->set_time_last(0);

	alert_name_map[arec->get_header()] = arec->get_alert_ref();
	alert_ref_map[arec->get_alert_ref()] = arec;

    alert_defs_vec.push_back(arec);

	return arec->get_alert_ref();
}

int Alertracker::FetchAlertRef(string in_header) {
    local_locker lock(&alert_mutex);

    if (alert_name_map.find(in_header) != alert_name_map.end())
        return alert_name_map[in_header];

    return -1;
}

int Alertracker::CheckTimes(shared_alert_def arec) {
	// Is this alert rate-limited?  If not, shortcut out and send it
	if (arec->get_limit_rate() == 0) {
		return 1;
	}

	struct timeval now;
	gettimeofday(&now, NULL);

	// If the last time we sent anything was longer than the main rate limit,
	// then we reset back to empty
	if (arec->get_time_last() < (now.tv_sec - 
                alert_time_unit_conv[arec->get_limit_unit()])) {
		arec->set_total_sent(0);
		arec->set_burst_sent(0);
		return 1;
	}

	// If the last time we sent anything was longer than the burst rate, we can
	// reset the burst to 0
	if (arec->get_time_last() < (now.tv_sec - 
                alert_time_unit_conv[arec->get_burst_unit()])) {
		arec->set_burst_sent(0);
	}

	// If we're under the limit on both, we're good to go
	if (arec->get_burst_sent() < arec->get_limit_burst() && 
            arec->get_total_sent() < arec->get_limit_rate())
		return 1;

	return 0;
}

int Alertracker::PotentialAlert(int in_ref) {
    local_locker lock(&alert_mutex);

	map<int, shared_alert_def>::iterator aritr = alert_ref_map.find(in_ref);

	if (aritr == alert_ref_map.end())
		return 0;

	shared_alert_def arec = aritr->second;

	return CheckTimes(arec);
}

int Alertracker::RaiseAlert(int in_ref, kis_packet *in_pack,
							mac_addr bssid, mac_addr source, mac_addr dest, 
							mac_addr other, string in_channel, string in_text) {
    local_locker lock(&alert_mutex);

	map<int, shared_alert_def>::iterator aritr = alert_ref_map.find(in_ref);

	if (aritr == alert_ref_map.end())
		return -1;

	shared_alert_def arec = aritr->second;

	if (CheckTimes(arec) != 1)
		return 0;

	kis_alert_info *info = new kis_alert_info;

	info->header = arec->get_header();
	info->phy = arec->get_phy();
	gettimeofday(&(info->tm), NULL);

	info->bssid = bssid;
	info->source = source;
	info->dest  = dest;
	info->other = other;

	info->channel = in_channel;	

	info->text = in_text;

	// Increment and set the timers
    arec->inc_burst_sent(1);
    arec->inc_total_sent(1);
    arec->set_time_last(time(0));

	alert_backlog.push_back(info);
	if ((int) alert_backlog.size() > num_backlog) {
		delete alert_backlog[0];
		alert_backlog.erase(alert_backlog.begin());
	}

	// Try to get the existing alert info
	if (in_pack != NULL)  {
		kis_alert_component *acomp = 
			(kis_alert_component *) in_pack->fetch(_PCM(PACK_COMP_ALERT));

		// if we don't have an alert container, make one on this packet
		if (acomp == NULL) {
			acomp = new kis_alert_component;
			in_pack->insert(_PCM(PACK_COMP_ALERT), acomp);
		}

		// Attach it to the packet
		acomp->alert_vec.push_back(info);
	}

	// Send the text info
	_MSG(info->header + " " + info->text, MSGFLAG_ALERT);

	return 1;
}

int Alertracker::ParseAlertStr(string alert_str, string *ret_name, 
							   alert_time_unit *ret_limit_unit, int *ret_limit_rate,
							   alert_time_unit *ret_limit_burst, 
							   int *ret_burst_rate) {
	char err[1024];
	vector<string> tokens = StrTokenize(alert_str, ",");

	if (tokens.size() != 3) {
		snprintf(err, 1024, "Malformed limits for alert '%s'", alert_str.c_str());
		globalreg->messagebus->InjectMessage(err, MSGFLAG_ERROR);
		return -1;
	}

	(*ret_name) = StrLower(tokens[0]);

	if (ParseRateUnit(StrLower(tokens[1]), ret_limit_unit, ret_limit_rate) != 1 ||
		ParseRateUnit(StrLower(tokens[2]), ret_limit_burst, ret_burst_rate) != 1) {
		snprintf(err, 1024, "Malformed limits for alert '%s'", alert_str.c_str());
		globalreg->messagebus->InjectMessage(err, MSGFLAG_ERROR);
		return -1;
	}

	return 1;
}

// Split up a rate/unit string into real values
int Alertracker::ParseRateUnit(string in_ru, alert_time_unit *ret_unit,
							   int *ret_rate) {
	vector<string> units = StrTokenize(in_ru, "/");

	if (units.size() == 1) {
		// Unit is per minute if not specified
		(*ret_unit) = sat_minute;
	} else {
		// Parse the string unit
		if (units[1] == "sec" || units[1] == "second") {
			(*ret_unit) = sat_second;
		} else if (units[1] == "min" || units[1] == "minute") {
			(*ret_unit) = sat_minute;
		} else if (units[1] == "hr" || units[1] == "hour") { 
			(*ret_unit) = sat_hour;
		} else if (units[1] == "day") {
			(*ret_unit) = sat_day;
		} else {
            _MSG("Invalid time unit for alert rate '" + units[1] + "'", 
                    MSGFLAG_ERROR);
			return -1;
		}
	}

	// Get the number
	if (sscanf(units[0].c_str(), "%d", ret_rate) != 1) {
        _MSG("Invalid rate '" + units[0] + "' for alert", MSGFLAG_ERROR);
		return -1;
	}

	return 1;
}

int Alertracker::ParseAlertConfig(ConfigFile *in_conf) {
	vector<string> clines = in_conf->FetchOptVec("alert");

	for (unsigned int x = 0; x < clines.size(); x++) {
		alert_conf_rec *rec = new alert_conf_rec;

		if (ParseAlertStr(clines[x], &(rec->header), &(rec->limit_unit), 
						  &(rec->limit_rate), &(rec->burst_unit), 
						  &(rec->limit_burst)) < 0) {
			_MSG("Invalid alert line in config file: " + clines[x], MSGFLAG_FATAL);
			globalreg->fatal_condition = 1;
            delete rec;
            return -1;
        }

		alert_conf_map[StrLower(rec->header)] = rec;
	}

	return 1;
}

int Alertracker::ActivateConfiguredAlert(string in_header, string in_desc) {
	return ActivateConfiguredAlert(in_header, in_desc, KIS_PHY_UNKNOWN);
}

int Alertracker::ActivateConfiguredAlert(string in_header, string in_desc, int in_phy) {
    alert_conf_rec *rec;

    {
        local_locker lock(&alert_mutex);

        string hdr = StrLower(in_header);

        if (alert_conf_map.find(hdr) == alert_conf_map.end()) {
            _MSG("Alert type " + string(in_header) + " not found in list of activated "
                    "alerts.", MSGFLAG_INFO);
            return -1;
        }

        rec = alert_conf_map[hdr];
    }

	return RegisterAlert(rec->header, in_desc, rec->limit_unit, rec->limit_rate, 
						 rec->burst_unit, rec->limit_burst, in_phy);
}

bool Alertracker::Httpd_VerifyPath(const char *path, const char *method) {
    if (strcmp(method, "GET") != 0) {
        return false;
    }

    if (!Httpd_CanSerialize(path))
        return false;

    // Split URL and process
    vector<string> tokenurl = StrTokenize(path, "/");
    if (tokenurl.size() < 3)
        return false;

    if (tokenurl[1] == "alerts") {
        if (Httpd_StripSuffix(tokenurl[2]) == "definitions") {
            return true;
        } else if (Httpd_StripSuffix(tokenurl[2]) == "all_alerts") {
            return true;
        } else if (tokenurl[2] == "last-time") {
            if (tokenurl.size() < 5)
                return false;

            if (Httpd_CanSerialize(tokenurl[4]))
                return true;

            return false;
        }
    }

    return false;
}

void Alertracker::Httpd_CreateStreamResponse(
        Kis_Net_Httpd *httpd __attribute__((unused)),
        Kis_Net_Httpd_Connection *connection,
        const char *path, const char *method, const char *upload_data,
        size_t *upload_data_size, std::stringstream &stream) {

    time_t since_time = 0;
    bool wrap = false;

    if (strcmp(method, "GET") != 0) {
        return;
    }

    if (!Httpd_CanSerialize(path))
        return;

    // Split URL and process
    vector<string> tokenurl = StrTokenize(path, "/");
    if (tokenurl.size() < 3)
        return;

    if (tokenurl[1] == "alerts") {
        if (Httpd_StripSuffix(tokenurl[2]) == "definitions") {
            Httpd_Serialize(path, stream, alert_defs);
            return;
        } else if (tokenurl[2] == "last-time") {
            if (tokenurl.size() < 5)
                return;

            long lastts;
            if (sscanf(tokenurl[3].c_str(), "%ld", &lastts) != 1)
                return;

            wrap = true;

            since_time = lastts;
        }
    }

    {
        local_locker lock(&alert_mutex);

        shared_ptr<TrackerElement> wrapper;
        shared_ptr<TrackerElement> msgvec(globalreg->entrytracker->GetTrackedInstance(alert_vec_id));

        // If we're doing a time-since, wrap the vector
        if (wrap) {
            wrapper.reset(new TrackerElement(TrackerMap));
            wrapper->add_map(msgvec);

            SharedTrackerElement ts(globalreg->entrytracker->GetTrackedInstance(alert_timestamp_id));
            ts->set((uint64_t) globalreg->timestamp.tv_sec);
            wrapper->add_map(ts);
        } else {
            wrapper = msgvec;
        }

        for (vector<kis_alert_info *>::iterator i = alert_backlog.begin();
                i != alert_backlog.end(); ++i) {
            if (since_time < (*i)->tm.tv_sec) {
                shared_ptr<tracked_alert> ta(new tracked_alert(globalreg, alert_entry_id));
                ta->from_alert_info(*i);
                msgvec->add_vector(ta);
            }
        }

        Httpd_Serialize(path, stream, wrapper);
    }
}

