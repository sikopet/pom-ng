/*
 *  This file is part of pom-ng.
 *  Copyright (C) 2014 Guy Martin <gmsoft@tuxicoman.be>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <pom-ng/analyzer_dtmf.h>
#include <pom-ng/proto_sip.h>
#include <pom-ng/ptype_bool.h>
#include <pom-ng/ptype_string.h>
#include <pom-ng/ptype_uint16.h>
#include <pom-ng/ptype_uint32.h>

#include "analyzer_sip.h"


#if 1
#define debug_sip(x ...) pomlog(POMLOG_DEBUG x)
#else
#define debug_sip(x ...)
#endif

struct analyzer_sip_call *analyzer_sip_calls = NULL;
static pthread_rwlock_t analyzer_sip_calls_lock = PTHREAD_RWLOCK_INITIALIZER;

struct mod_reg_info* analyzer_sip_reg_info() {

	static struct mod_reg_info reg_info;
	memset(&reg_info, 0, sizeof(struct mod_reg_info));
	reg_info.api_ver = MOD_API_VER;
	reg_info.register_func = analyzer_sip_mod_register;
	reg_info.unregister_func = analyzer_sip_mod_unregister;
	reg_info.dependencies = "analyzer_sdp, proto_sip, ptype_string";

	return &reg_info;
}

static int analyzer_sip_mod_register(struct mod_reg *mod) {

	static struct analyzer_reg analyzer_sip;
	memset(&analyzer_sip, 0, sizeof(struct analyzer_reg));
	analyzer_sip.name = "sip";
	analyzer_sip.mod = mod;
	analyzer_sip.init = analyzer_sip_init;
	analyzer_sip.finish = analyzer_sip_finish;
	analyzer_sip.cleanup = analyzer_sip_cleanup;

	return analyzer_register(&analyzer_sip);

}

static int analyzer_sip_mod_unregister() {

	return analyzer_unregister("sip");
}

static int analyzer_sip_init(struct analyzer *analyzer) {

	struct analyzer_sip_priv *priv = malloc(sizeof(struct analyzer_sip_priv));
	if (!priv) {
		pom_oom(sizeof(struct analyzer_sip_priv));
		return POM_ERR;
	}

	memset(priv, 0, sizeof(struct analyzer_sip_priv));
	analyzer->priv = priv;

	struct registry_param *p = NULL;

	priv->evt_sip_req = event_find("sip_req");
	priv->evt_sip_rsp = event_find("sip_rsp");
	if (!priv->evt_sip_req || !priv->evt_sip_rsp)
		goto err;

	priv->p_dialog_timeout = ptype_alloc_unit("uint32", "seconds");
	priv->p_call_max_duration = ptype_alloc_unit("uint32", "seconds");
	if (!priv->p_dialog_timeout || !priv->p_call_max_duration)
		goto err;

	p = registry_new_param("dialog_timeout", "300", priv->p_dialog_timeout, "Timeout for SIP dialogs", 0);
	if (analyzer_add_param(analyzer, p) != POM_OK)
		goto err;

	p = registry_new_param("call_max_duration", "10800", priv->p_call_max_duration, "Maximum duration of established calls", 0);
	if (analyzer_add_param(analyzer, p) != POM_OK)
		goto err;

	p = NULL;

	static struct data_item_reg evt_sip_call_common_data_items[ANALYZER_SIP_CALL_COMMON_DATA_COUNT] = { { 0 } };

	evt_sip_call_common_data_items[analyzer_sip_call_common_from_display].name = "from_display";
	evt_sip_call_common_data_items[analyzer_sip_call_common_from_display].value_type = ptype_get_type("string");

	evt_sip_call_common_data_items[analyzer_sip_call_common_from_uri].name = "from_uri";
	evt_sip_call_common_data_items[analyzer_sip_call_common_from_uri].value_type = ptype_get_type("string");

	evt_sip_call_common_data_items[analyzer_sip_call_common_to_display].name = "to_display";
	evt_sip_call_common_data_items[analyzer_sip_call_common_to_display].value_type = ptype_get_type("string");

	evt_sip_call_common_data_items[analyzer_sip_call_common_to_uri].name = "to_uri";
	evt_sip_call_common_data_items[analyzer_sip_call_common_to_uri].value_type = ptype_get_type("string");

	evt_sip_call_common_data_items[analyzer_sip_call_common_id].name = "call_id";
	evt_sip_call_common_data_items[analyzer_sip_call_common_id].value_type = ptype_get_type("string");

	static struct data_reg evt_sip_call_common_data = {
		.items = evt_sip_call_common_data_items,
		.data_count = ANALYZER_SIP_CALL_COMMON_DATA_COUNT
	};

	static struct data_item_reg evt_sip_call_data_items[ANALYZER_SIP_CALL_DATA_COUNT] = { { 0 } };
	memcpy(evt_sip_call_data_items, evt_sip_call_common_data_items, sizeof(struct data_item_reg) * ANALYZER_SIP_CALL_COMMON_DATA_COUNT);

	evt_sip_call_data_items[analyzer_sip_call_trying_duration].name = "trying_duration";
	evt_sip_call_data_items[analyzer_sip_call_trying_duration].value_type = ptype_get_type("uint32");

	evt_sip_call_data_items[analyzer_sip_call_ringing_duration].name = "ringing_duration";
	evt_sip_call_data_items[analyzer_sip_call_ringing_duration].value_type = ptype_get_type("uint32");

	evt_sip_call_data_items[analyzer_sip_call_connected_duration].name = "connected_duration";
	evt_sip_call_data_items[analyzer_sip_call_connected_duration].value_type = ptype_get_type("uint32");

	static struct data_reg evt_sip_call_data = {
		.items = evt_sip_call_data_items,
		.data_count = ANALYZER_SIP_CALL_DATA_COUNT
	};

	static struct event_reg_info analyzer_sip_evt_call = { 0 };
	analyzer_sip_evt_call.source_name = "analyzer_sip";
	analyzer_sip_evt_call.source_obj = analyzer;
	analyzer_sip_evt_call.name = "sip_call";
	analyzer_sip_evt_call.description = "Complete SIP call";
	analyzer_sip_evt_call.data_reg = &evt_sip_call_data;
	analyzer_sip_evt_call.flags = EVENT_REG_FLAG_PAYLOAD;
	analyzer_sip_evt_call.listeners_notify = analyzer_sip_event_listeners_notify;

	priv->evt_sip_call = event_register(&analyzer_sip_evt_call);
	if (!priv->evt_sip_call)
		goto err;

	static struct event_reg_info analyzer_sip_evt_call_dial = { 0 };
	analyzer_sip_evt_call_dial.source_name = "analyzer_sip";
	analyzer_sip_evt_call_dial.source_obj = analyzer;
	analyzer_sip_evt_call_dial.name = "sip_call_dial";
	analyzer_sip_evt_call_dial.description = "A SIP call is dial";
	analyzer_sip_evt_call_dial.data_reg = &evt_sip_call_common_data;
	analyzer_sip_evt_call_dial.listeners_notify = analyzer_sip_event_listeners_notify;

	priv->evt_sip_call_dial = event_register(&analyzer_sip_evt_call_dial);
	if (!priv->evt_sip_call_dial)
		goto err;

	static struct event_reg_info analyzer_sip_evt_call_ringing = { 0 };
	analyzer_sip_evt_call_ringing.source_name = "analyzer_sip";
	analyzer_sip_evt_call_ringing.source_obj = analyzer;
	analyzer_sip_evt_call_ringing.name = "sip_call_ringing";
	analyzer_sip_evt_call_ringing.description = "A SIP call is ringing";
	analyzer_sip_evt_call_ringing.data_reg = &evt_sip_call_common_data;
	analyzer_sip_evt_call_ringing.listeners_notify = analyzer_sip_event_listeners_notify;

	priv->evt_sip_call_ringing = event_register(&analyzer_sip_evt_call_ringing);
	if (!priv->evt_sip_call_ringing)
		goto err;

	static struct event_reg_info analyzer_sip_evt_call_connect = { 0 };
	analyzer_sip_evt_call_connect.source_name = "analyzer_sip";
	analyzer_sip_evt_call_connect.source_obj = analyzer;
	analyzer_sip_evt_call_connect.name = "sip_call_connect";
	analyzer_sip_evt_call_connect.description = "A SIP call is connected";
	analyzer_sip_evt_call_connect.data_reg = &evt_sip_call_common_data;
	analyzer_sip_evt_call_connect.listeners_notify = analyzer_sip_event_listeners_notify;

	priv->evt_sip_call_connect = event_register(&analyzer_sip_evt_call_connect);
	if (!priv->evt_sip_call_connect)
		goto err;

	static struct event_reg_info analyzer_sip_evt_call_hangup = { 0 };
	analyzer_sip_evt_call_hangup.source_name = "analyzer_sip";
	analyzer_sip_evt_call_hangup.source_obj = analyzer;
	analyzer_sip_evt_call_hangup.name = "sip_call_hangup";
	analyzer_sip_evt_call_hangup.description = "A SIP call is hanged up";
	analyzer_sip_evt_call_hangup.data_reg = &evt_sip_call_common_data;
	analyzer_sip_evt_call_hangup.listeners_notify = analyzer_sip_event_listeners_notify;

	priv->evt_sip_call_hangup = event_register(&analyzer_sip_evt_call_hangup);
	if (!priv->evt_sip_call_hangup)
		goto err;

	static struct data_item_reg evt_sip_dtmf_data_items[ANALYZER_SIP_CALL_DTMF_DATA_COUNT] = { { 0 } };
	memcpy(evt_sip_dtmf_data_items, evt_sip_call_common_data_items, sizeof(struct data_item_reg) * ANALYZER_SIP_CALL_COMMON_DATA_COUNT);

	evt_sip_dtmf_data_items[analyzer_sip_dtmf_signal].name = "signal";
	evt_sip_dtmf_data_items[analyzer_sip_dtmf_signal].value_type = ptype_get_type("string");

	evt_sip_dtmf_data_items[analyzer_sip_dtmf_duration].name = "duration";
	evt_sip_dtmf_data_items[analyzer_sip_dtmf_duration].value_type = ptype_get_type("uint16");

	static struct data_reg evt_sip_dtmf_data = {
		.items = evt_sip_dtmf_data_items,
		.data_count = ANALYZER_SIP_CALL_DTMF_DATA_COUNT
	};

	static struct event_reg_info analyzer_sip_evt_dtmf = { 0 };
	analyzer_sip_evt_dtmf.source_name = "analyzer_sip";
	analyzer_sip_evt_dtmf.source_obj = analyzer;
	analyzer_sip_evt_dtmf.name = "sip_dtmf";
	analyzer_sip_evt_dtmf.description = "DTMF from SIP";
	analyzer_sip_evt_dtmf.data_reg = &evt_sip_dtmf_data;
	analyzer_sip_evt_dtmf.listeners_notify = analyzer_sip_event_listeners_notify;

	priv->evt_sip_dtmf = event_register(&analyzer_sip_evt_dtmf);
	if (!priv->evt_sip_dtmf)
		goto err;

	priv->proto_sip = proto_get("sip");
	if (!priv->proto_sip)
		goto err;

	return POM_OK;
err:

	if (p)
		registry_cleanup_param(p);

	analyzer_sip_cleanup(analyzer);
	return POM_ERR;
}

static int analyzer_sip_finish(struct analyzer *analyzer) {

	struct analyzer_sip_call *cur_call, *tmp;
	HASH_ITER(hh, analyzer_sip_calls, cur_call, tmp) {
		HASH_DEL(analyzer_sip_calls, cur_call);
		analyzer_sip_call_cleanup(cur_call);
	}

	return POM_OK;
}

static int analyzer_sip_cleanup(struct analyzer *analyzer) {

	struct analyzer_sip_priv *priv = analyzer->priv;

	if (priv->listening) {
		event_listener_unregister(priv->evt_sip_req, analyzer);
		event_listener_unregister(priv->evt_sip_rsp, analyzer);
	}

	if (priv->sdp_listening)
		pload_listen_stop(analyzer, ANALYZER_SIP_SDP_PLOAD_TYPE);

	if (priv->dtmf_listening)
		pload_listen_stop(analyzer, ANALYZER_SIP_DTMF_PLOAD_TYPE);

	if (priv->evt_sip_call)
		event_unregister(priv->evt_sip_call);

	if (priv->evt_sip_call_dial)
		event_unregister(priv->evt_sip_call_dial);

	if (priv->evt_sip_call_ringing)
		event_unregister(priv->evt_sip_call_ringing);

	if (priv->evt_sip_call_connect)
		event_unregister(priv->evt_sip_call_connect);

	if (priv->evt_sip_call_hangup)
		event_unregister(priv->evt_sip_call_hangup);

	if (priv->evt_sip_dtmf)
		event_unregister(priv->evt_sip_dtmf);

	if (priv->p_dialog_timeout)
		ptype_cleanup(priv->p_dialog_timeout);

	if (priv->p_call_max_duration)
		ptype_cleanup(priv->p_call_max_duration);

	free(priv);

	return POM_OK;

}

static int analyzer_sip_event_listeners_notify(void *obj, struct event_reg *evt_reg, int has_listeners) {


	struct analyzer *analyzer = obj;
	struct analyzer_sip_priv *priv = analyzer->priv;

	int listening;
	if (has_listeners)
		listening = __sync_add_and_fetch(&priv->listening, 1);
	else
		listening = __sync_sub_and_fetch(&priv->listening, 1);

	if (has_listeners && listening == 1) {
		if (event_listener_register(priv->evt_sip_req, analyzer, analyzer_sip_event_process_begin, NULL) != POM_OK) {
			return POM_ERR;
		} else if (event_listener_register(priv->evt_sip_rsp, analyzer, analyzer_sip_event_process_begin, NULL) != POM_OK) {
			event_listener_unregister(priv->evt_sip_req, analyzer);
			return POM_ERR;
		}

	} else if (!listening) {
		int res = POM_OK;
		res += event_listener_unregister(priv->evt_sip_req, analyzer);
		res += event_listener_unregister(priv->evt_sip_rsp, analyzer);
		if (res != POM_OK)
			return POM_ERR;
	}

	if (event_has_listener(priv->evt_sip_call)) {
		if (!priv->sdp_listening) {
			if (pload_listen_start(obj, ANALYZER_SIP_SDP_PLOAD_TYPE, NULL, analyzer_sip_sdp_open, analyzer_sip_sdp_write, analyzer_sip_sdp_close) != POM_OK)
				return POM_ERR;
			priv->sdp_listening = 1;
		}
	} else {
		if (priv->sdp_listening) {
			if (pload_listen_stop(obj, ANALYZER_SIP_SDP_PLOAD_TYPE) != POM_OK)
				return POM_ERR;
			priv->sdp_listening = 0;
		}
	}

	if (event_has_listener(priv->evt_sip_dtmf)) {
		if (!priv->dtmf_listening) {
			if (pload_listen_start(obj, ANALYZER_SIP_DTMF_PLOAD_TYPE, NULL, analyzer_sip_dtmf_open, NULL, NULL) != POM_OK)
				return POM_ERR;
			priv->dtmf_listening = 1;
		}
	} else {
		if (priv->dtmf_listening) {
			if (pload_listen_stop(obj, ANALYZER_SIP_DTMF_PLOAD_TYPE) != POM_OK)
				return POM_ERR;
			priv->dtmf_listening = 0;
		}
	}
	return POM_OK;
}

static struct analyzer_sip_call* analyzer_sip_event_get_call(struct analyzer *a, struct event *evt) {

	struct data *evt_data = event_get_data(evt);
	if (!data_is_set(evt_data[proto_sip_msg_call_id]))
		return NULL;

	struct conntrack_entry *ce = event_get_conntrack(evt);
	if (!ce)
		return NULL;

	char *call_id = PTYPE_STRING_GETVAL(evt_data[proto_sip_msg_call_id].value);

	struct analyzer_sip_call *call = NULL;

	pom_rwlock_rlock(&analyzer_sip_calls_lock);
	HASH_FIND_STR(analyzer_sip_calls, call_id, call);

	if (call) {
		pom_mutex_lock(&call->lock);
		pom_rwlock_unlock(&analyzer_sip_calls_lock);
		return call;
	}

	pom_rwlock_unlock(&analyzer_sip_calls_lock);


	// Write relock
	pom_rwlock_wlock(&analyzer_sip_calls_lock);

	// Doublecheck that the call hasn't been created
	HASH_FIND_STR(analyzer_sip_calls, call_id, call);
	if (call) {
		pom_mutex_lock(&call->lock);
		pom_rwlock_unlock(&analyzer_sip_calls_lock);
		return call;
	}

	debug_sip("Creating new call %s", call_id);

	// The call wasn't found, create it and bind it to the session
	call = malloc(sizeof(struct analyzer_sip_call));
	if (!call) {
		pom_oom(sizeof(struct analyzer_sip_call));
		return NULL;
	}
	memset(call, 0, sizeof(struct analyzer_sip_call));

	int res = pthread_mutex_init(&call->lock, NULL);
	if (res) {
		free(call);
		pomlog(POMLOG_ERR "Error while initializing the call lock : %s", pom_strerror(res));
		return NULL;
	}

	call->t = timer_alloc(call, analyzer_sip_call_timeout);
	if (!call->t)
		goto err;

	call->call_id = strdup(call_id);
	if (!call->call_id) {
		pom_oom(strlen(call_id) + 1);
		goto err;
	}

	// Add the call to the hash table
	HASH_ADD_KEYPTR(hh, analyzer_sip_calls, call->call_id, strlen(call->call_id), call);
	pom_mutex_lock(&call->lock);
	pom_rwlock_unlock(&analyzer_sip_calls_lock);

	return call;

err:
	pom_rwlock_unlock(&analyzer_sip_calls_lock);
	if (call) {
		pthread_mutex_destroy(&call->lock);
		if (call->call_id)
			free(call->call_id);
		if (call->t)
			timer_cleanup(call->t);
		free(call);
	}

	return NULL;
}

static int analyzer_sip_conntrack_cleanup(void *obj, void *priv) {

	struct analyzer_sip_conntrack_priv *cpriv = priv;

	while (cpriv->dialogs) {
		struct analyzer_sip_call_dialog *d = cpriv->dialogs;
		cpriv->dialogs = d->ce_next;
		analyzer_sip_dialog_cleanup(d);
	}

	free(cpriv);
	return POM_OK;
}

static int analyzer_sip_call_cleanup(struct analyzer_sip_call *call) {

	if (call->dialogs) {
		pomlog(POMLOG_WARN "Not cleaning up call with remaining dialogs !");
		return POM_ERR;
	}

	debug_sip("Cleaning up call %s", call->call_id);

	if (call->evt) {
		if (event_is_started(call->evt))
			event_process_end(call->evt);
		else
			event_cleanup(call->evt);
	}

	if (call->call_id)
		free(call->call_id);

	if (call->t)
		timer_cleanup(call->t);

	if (call->tel_call)
		telephony_call_cleanup(call->tel_call);

	pthread_mutex_destroy(&call->lock);

	free(call);

	return POM_OK;
}

static int analyzer_sip_call_dialog_terminate(struct analyzer_sip_priv *priv, struct event *evt, struct analyzer_sip_call_dialog *d) {

	d->terminated = 1;
	return POM_OK;
}

static void analyzer_sip_event_common_data_copy(struct data *dst_data, struct data *src_data) {

	// Source data must be from a proto_sip event
	// Destination data must be from an analyzer_sip event

	if (data_is_set(src_data[proto_sip_msg_from_display]))
		if (ptype_copy(dst_data[analyzer_sip_call_common_from_display].value, src_data[proto_sip_msg_from_display].value) == POM_OK)
			data_set(dst_data[analyzer_sip_call_common_from_display]);
	if (data_is_set(src_data[proto_sip_msg_from_uri]))
		if (ptype_copy(dst_data[analyzer_sip_call_common_from_uri].value, src_data[proto_sip_msg_from_uri].value) == POM_OK)
			data_set(dst_data[analyzer_sip_call_common_from_uri]);

	if (data_is_set(src_data[proto_sip_msg_to_display]))
		if (ptype_copy(dst_data[analyzer_sip_call_common_to_display].value, src_data[proto_sip_msg_to_display].value) == POM_OK)
			data_set(dst_data[analyzer_sip_call_common_to_display]);

	if (data_is_set(src_data[proto_sip_msg_to_uri]))
		if (ptype_copy(dst_data[analyzer_sip_call_common_to_uri].value, src_data[proto_sip_msg_to_uri].value) == POM_OK)
			data_set(dst_data[analyzer_sip_call_common_to_uri]);

	if (ptype_copy(dst_data[analyzer_sip_call_common_id].value, src_data[proto_sip_msg_call_id].value) == POM_OK)
		data_set(dst_data[analyzer_sip_call_common_id]);

}

static int analyzer_sip_call_set_state(struct analyzer_sip_priv *priv, struct event *evt, struct analyzer_sip_call *call, enum analyzer_sip_call_state state) {

	if (call->usage != analyzer_sip_call_usage_invite) {
		// We don't care about the state if it's not for a an INVITE
		return POM_OK;
	}

	if (call->state == state)
		return POM_OK; // Still the same state

	if (state < call->state) {
		debug_sip("Call state cannot go down !");
		return POM_OK;
	}

	struct data *evt_src_data = event_get_data(evt);

	if (state >= analyzer_sip_call_state_trying && call->state < analyzer_sip_call_state_trying) {
		// We have a new call !
		if (event_has_listener(priv->evt_sip_call_dial)) {
			struct event *tmp_evt = event_alloc(priv->evt_sip_call_dial);
			if (!tmp_evt)
				return POM_ERR;
			struct data *evt_dst_data = event_get_data(tmp_evt);
			analyzer_sip_event_common_data_copy(evt_dst_data, evt_src_data);
			if (event_process(tmp_evt, NULL, 0, event_get_timestamp(evt)) != POM_OK)
				return POM_ERR;
		}

		if (event_has_listener(priv->evt_sip_call)) {
			call->evt = event_alloc(priv->evt_sip_call);
			if (!call->evt)
				return POM_ERR;

			struct data *evt_dst_data = event_get_data(call->evt);
			analyzer_sip_event_common_data_copy(evt_dst_data, evt_src_data);

			if (event_process_begin(call->evt, NULL, 0, event_get_timestamp(evt)) != POM_OK)
				return POM_ERR;
		}


		call->start_ts = event_get_timestamp(evt);
	}

	if (state == analyzer_sip_call_state_alerting) {
		call->ringing_ts = event_get_timestamp(evt);

		if (call->evt) {
			struct data *evt_data = event_get_data(call->evt);
			PTYPE_UINT32_SETVAL(evt_data[analyzer_sip_call_trying_duration].value, pom_ptime_sec(call->ringing_ts - call->start_ts));
			data_set(evt_data[analyzer_sip_call_trying_duration]);
		}

		if (event_has_listener(priv->evt_sip_call_ringing)) {
			struct event *tmp_evt = event_alloc(priv->evt_sip_call_ringing);
			if (!tmp_evt)
				return POM_ERR;
			struct data *evt_dst_data = event_get_data(tmp_evt);
			analyzer_sip_event_common_data_copy(evt_dst_data, evt_src_data);
			if (event_process(tmp_evt, NULL, 0, event_get_timestamp(evt)) != POM_OK)
				return POM_ERR;
		}

	}

	if (state == analyzer_sip_call_state_connected) {

		// Start the connected timer
		timer_queue_now(call->t, *PTYPE_UINT32_GETVAL(priv->p_call_max_duration), event_get_timestamp(evt));

		call->connected_ts = event_get_timestamp(evt);

		if (call->evt) {
			struct data *evt_data = event_get_data(call->evt);

			if (call->ringing_ts) {
				PTYPE_UINT32_SETVAL(evt_data[analyzer_sip_call_ringing_duration].value, pom_ptime_sec(call->connected_ts - call->ringing_ts));
				data_set(evt_data[analyzer_sip_call_ringing_duration]);
			} else {
				PTYPE_UINT32_SETVAL(evt_data[analyzer_sip_call_trying_duration].value, pom_ptime_sec(call->connected_ts - call->start_ts));
				data_set(evt_data[analyzer_sip_call_trying_duration]);
			}
		}

		if (event_has_listener(priv->evt_sip_call_connect)) {
			struct event *tmp_evt = event_alloc(priv->evt_sip_call_connect);
			if (!tmp_evt)
				return POM_ERR;
			struct data *evt_dst_data = event_get_data(tmp_evt);
			analyzer_sip_event_common_data_copy(evt_dst_data, evt_src_data);
			if (event_process(tmp_evt, NULL, 0, event_get_timestamp(evt)) != POM_OK)
				return POM_ERR;
		}

	}

	if (state == analyzer_sip_call_state_terminated) {
		if (call->evt) {
			ptime ts = event_get_timestamp(evt);
			struct data *evt_data = event_get_data(call->evt);
			if (call->connected_ts) {
				PTYPE_UINT32_SETVAL(evt_data[analyzer_sip_call_connected_duration].value, pom_ptime_sec(ts - call->connected_ts));
				data_set(evt_data[analyzer_sip_call_connected_duration]);
			} else if (call->ringing_ts) {
				PTYPE_UINT32_SETVAL(evt_data[analyzer_sip_call_ringing_duration].value, pom_ptime_sec(ts - call->ringing_ts));
				data_set(evt_data[analyzer_sip_call_ringing_duration]);
			} else {
				PTYPE_UINT32_SETVAL(evt_data[analyzer_sip_call_trying_duration].value, pom_ptime_sec(ts - call->start_ts));
				data_set(evt_data[analyzer_sip_call_trying_duration]);
			}

			event_process_end(call->evt);
			call->evt = NULL;
		}

		if (event_has_listener(priv->evt_sip_call_hangup)) {
			struct event *tmp_evt = event_alloc(priv->evt_sip_call_hangup);
			if (!tmp_evt)
				return POM_ERR;
			struct data *evt_dst_data = event_get_data(tmp_evt);
			analyzer_sip_event_common_data_copy(evt_dst_data, evt_src_data);
			if (event_process(tmp_evt, NULL, 0, event_get_timestamp(evt)) != POM_OK)
				return POM_ERR;
		}
	}

	call->state = state;

	return POM_OK;
}

static int analyzer_sip_process_request(struct analyzer_sip_priv *priv, struct event *evt, struct analyzer_sip_call_dialog *d, enum analyzer_sip_method method, char *method_str) {

	if (method == analyzer_sip_method_unknown)
		return POM_OK;

	if (d->call->usage == analyzer_sip_call_usage_other) {
		d->call->usage = analyzer_sip_call_usage_invite;
	} else if (d->call->usage != analyzer_sip_call_usage_invite) {
		debug_sip("Received %s on a call not used for INVITE, ignoring", method_str);
		return POM_OK;
	}

	if (method == analyzer_sip_method_invite) {
		if (d->call->state > analyzer_sip_call_state_trying) {
			debug_sip("Got re-INVITE for call id %s", d->call->call_id);
			return POM_OK;
		}
		return analyzer_sip_call_set_state(priv, evt, d->call, analyzer_sip_call_state_trying);
	} else if (method == analyzer_sip_method_cancel) {
		// The current dialog is canceled
		return analyzer_sip_call_dialog_terminate(priv, evt, d);
	} else if (method == analyzer_sip_method_bye) {
		return analyzer_sip_call_set_state(priv, evt, d->call, analyzer_sip_call_state_terminated);
	}

	return POM_OK;
}

static enum analyzer_sip_method analyzer_sip_method_get_id(char *method_str) {

	if (!strcmp(method_str, "INVITE")) {
		return analyzer_sip_method_invite;
	} else if (!strcmp(method_str, "ACK")) {
		return analyzer_sip_method_ack;
	} else if (!strcmp(method_str, "CANCEL")) {
		return analyzer_sip_method_cancel;
	} else if (!strcmp(method_str, "BYE")) {
		return analyzer_sip_method_bye;
	}

	pomlog(POMLOG_DEBUG "Unknown SIP method %s", method_str);

	return analyzer_sip_method_unknown;
}

static int analyzer_sip_process_response(struct analyzer_sip_priv *priv, struct event *evt, struct analyzer_sip_call_dialog *d, uint16_t status) {

	if (status == 180) { // Ringing
		return analyzer_sip_call_set_state(priv, evt, d->call, analyzer_sip_call_state_alerting);
	} else if (status == 200) {
		if (d->cseq_method == analyzer_sip_method_invite)
			return analyzer_sip_call_set_state(priv, evt, d->call, analyzer_sip_call_state_connected);
	}
	return POM_OK;
}

static int analyzer_sip_event_process_begin(struct event *evt, void *obj, struct proto_process_stack *stack, unsigned int stack_index) {

	struct analyzer *analyzer = obj;
	struct analyzer_sip_priv *priv = analyzer->priv;

	struct event_reg *evt_reg = event_get_reg(evt);

	if (evt_reg != priv->evt_sip_req && evt_reg != priv->evt_sip_rsp) {
		pomlog(POMLOG_ERR "Received event is not a SIP request or response");
		return POM_ERR;
	}

	// Get the call based on the call-id
	struct analyzer_sip_call *call = analyzer_sip_event_get_call(obj, evt);
	if (!call)
		return POM_OK;

	struct data *evt_data = event_get_data(evt);

	if (!data_is_set(evt_data[proto_sip_msg_top_branch])) {
		pom_mutex_unlock(&call->lock);
		pomlog(POMLOG_DEBUG "Received SIP event without branch, ignoring");
		return POM_OK;
	}
	if (!data_is_set(evt_data[proto_sip_msg_from_tag])) {
		pom_mutex_unlock(&call->lock);
		pomlog(POMLOG_DEBUG "Received SIP event without from tag, ignoring");
		return POM_OK;
	}

	// Find or create a dialog for this message

	char *branch = PTYPE_STRING_GETVAL(evt_data[proto_sip_msg_top_branch].value);
	char *from_tag = PTYPE_STRING_GETVAL(evt_data[proto_sip_msg_from_tag].value);
	char *to_tag = NULL;
	if (data_is_set(evt_data[proto_sip_msg_to_tag]))
		to_tag = PTYPE_STRING_GETVAL(evt_data[proto_sip_msg_to_tag].value);

	uint32_t cseq = *PTYPE_UINT32_GETVAL(evt_data[proto_sip_msg_cseq_num].value);

	struct conntrack_entry *ce = event_get_conntrack(evt);

	struct analyzer_sip_conntrack_priv *cpriv = conntrack_get_priv(ce, analyzer);

	if (!cpriv) {
		cpriv = malloc(sizeof(struct analyzer_sip_conntrack_priv));
		if (!cpriv) {
			pom_mutex_unlock(&call->lock);
			pom_oom(sizeof(struct analyzer_sip_conntrack_priv));
			return POM_ERR;
		}
		memset(cpriv, 0, sizeof(struct analyzer_sip_conntrack_priv));
		conntrack_add_priv(ce, analyzer, cpriv, analyzer_sip_conntrack_cleanup);
	}

	struct analyzer_sip_call_dialog *dialog = NULL;

	if (to_tag) {
		// This might be the first message creating an complete dialog or
		// a message from the UAS like a re-invite
		for (dialog = cpriv->dialogs; dialog; dialog = dialog->next) {
			// Try to match the from tag
			if (!strcmp(from_tag, dialog->from_tag)) {
				// From tag matched,
				if (dialog->to_tag) {
					if (!strcmp(to_tag, dialog->to_tag))
						break; // Both from and to tag matched !
				} else {
					if (!strcmp(branch, dialog->branch)) {
						// We matched both the from and the branch this means
						// we are transitioning from half to full dialog
						dialog->to_tag = strdup(to_tag);
						if (!dialog->to_tag) {
							pom_mutex_unlock(&call->lock);
							pom_oom(strlen(to_tag) + 1);
							return POM_ERR;
						}
						debug_sip("Half dialog for call id %s transitioned to full : from_tag %s, to_tag %s, branch %s", call->call_id, from_tag, to_tag, branch);
						break;
					}
				}
			} else if (!strcmp(to_tag, dialog->from_tag)) {
				// The dialog matched in the reverse direction
				// This means we are processing a request from the UAS or a response from the UAC
				break;
			}
		}
	} else {
		// Half dialog
		for (dialog = cpriv->dialogs; dialog ; dialog = dialog->next) {
			// Try to match the from tag
			if (strcmp(from_tag, dialog->from_tag))
				continue;

			// If it matches, check the branch
			if (!strcmp(branch, dialog->branch))
				break; // Found it
		}
	}

	if (!dialog) {
		// No dialog was found, let's create one !
		dialog = malloc(sizeof(struct analyzer_sip_call_dialog));
		if (!dialog) {
			pom_mutex_unlock(&call->lock);
			pom_oom(sizeof(struct analyzer_sip_call_dialog));
			return POM_ERR;
		}
		memset(dialog, 0, sizeof(struct analyzer_sip_call_dialog));

		dialog->ce = ce;

		dialog->t = conntrack_timer_alloc(ce, analyzer_sip_dialog_timeout, dialog);
		if (!dialog->t) {
			pom_mutex_unlock(&call->lock);
			return POM_ERR;
		}

		dialog->from_tag = strdup(from_tag);
		if (!dialog->from_tag) {
			pom_mutex_unlock(&call->lock);
			conntrack_timer_cleanup(dialog->t);
			free(dialog);
			pom_oom(strlen(from_tag) + 1);
			pom_mutex_unlock(&call->lock);
			return POM_ERR;
		}

		dialog->branch = strdup(branch);
		if (!dialog->branch) {
			pom_mutex_unlock(&call->lock);
			free(dialog->from_tag);
			conntrack_timer_cleanup(dialog->t);
			free(dialog);
			pom_oom(strlen(branch) + 1);
			return POM_ERR;
		}

		if (to_tag) {
			dialog->to_tag = strdup(to_tag);
			if (!dialog->to_tag) {
				pom_mutex_unlock(&call->lock);
				free(dialog->branch);
				free(dialog->from_tag);
				conntrack_timer_cleanup(dialog->t);
				free(dialog);
				pom_oom(strlen(to_tag) + 1);
				return POM_ERR;
			}
		}

		if (to_tag) {
			debug_sip("New full dialog for call %s : from_tag %s, to_tag %s, branch %s", call->call_id, from_tag, to_tag, branch);
		} else {
			debug_sip("New half dialog for call %s : from_tag %s, branch %s", call->call_id, from_tag, branch);
		}

		dialog->call = call;
		dialog->cseq = cseq - 1;

		// Add the dialog to the call
		dialog->next = call->dialogs;
		if (dialog->next)
			dialog->next->prev = dialog;
		call->dialogs = dialog;

		// Add the dialog to the conntrack_priv
		dialog->ce_next = cpriv->dialogs;
		if (dialog->ce_next)
			dialog->ce_next->ce_prev = dialog;
		cpriv->dialogs = dialog;
	}

	cpriv->cur_dialog = dialog;

	if (dialog->terminated) {
		pom_mutex_unlock(&call->lock);
		debug_sip("Ignoring event on terminated dialog for call %s", call->call_id);
		return POM_OK;
	}

	if (conntrack_timer_queue(dialog->t, *PTYPE_UINT32_GETVAL(priv->p_dialog_timeout), event_get_timestamp(evt)) != POM_OK) {
		pom_mutex_unlock(&call->lock);
		return POM_ERR;
	}

	int res = POM_OK;
	if (evt_reg == priv->evt_sip_req) {

		char *method_str = PTYPE_STRING_GETVAL(evt_data[proto_sip_req_method].value);

		enum analyzer_sip_method method = analyzer_sip_method_get_id(method_str);

		if (dialog->cseq >= cseq) {

			if (dialog->cseq > cseq || (method != analyzer_sip_method_ack && method != analyzer_sip_method_cancel && method != analyzer_sip_method_bye)) {
				pom_mutex_unlock(&call->lock);
				debug_sip("Ignoring %s because it's an old or retransmitted request : cur cseq %u, req cseq %u", method_str, dialog->cseq, cseq);
				return POM_OK;
			}
		}

		if (method != analyzer_sip_method_unknown)
			res = analyzer_sip_process_request(priv, evt, dialog, method, method_str);

		dialog->cseq = cseq;
		dialog->cseq_method = method;


	} else {

		uint16_t status = *PTYPE_UINT16_GETVAL(evt_data[proto_sip_rsp_status].value);
		if (dialog->cseq > cseq) {
			pom_mutex_unlock(&call->lock);
			debug_sip("Ignoring response %hu because it's an old or retransmitted response : cur cseq %u, rsp cseq %u", status, dialog->cseq, cseq);
			return POM_OK;
		}

		res = analyzer_sip_process_response(priv, evt, dialog, status);

	}
	pom_mutex_unlock(&call->lock);
	return res;
}

static int analyzer_sip_call_timeout(void *priv, ptime now) {

	// An established call lasted more than our max duration
	struct analyzer_sip_call *call = priv;

	pom_rwlock_wlock(&analyzer_sip_calls_lock);
	HASH_DEL(analyzer_sip_calls, call);
	pom_mutex_lock(&call->lock);
	pom_mutex_unlock(&call->lock);
	pom_rwlock_unlock(&analyzer_sip_calls_lock);

	return analyzer_sip_call_cleanup(call);
}

static int analyzer_sip_dialog_timeout(struct conntrack_entry *ce, void *priv, ptime now) {

	struct analyzer_sip_call_dialog *d = priv;
	struct analyzer_sip_conntrack_priv *cpriv = d->ce_priv;

	// Remove the dialog from the conntrack_entry so we can release the conntrack
	if (d->ce_prev)
		d->ce_prev->ce_next = d->ce_next;
	else
		cpriv->dialogs = d->ce_next;

	if (d->ce_next)
		d->ce_next->ce_prev = d->ce_prev;

	conntrack_unlock(ce);

	return analyzer_sip_dialog_cleanup(d);
}

static int analyzer_sip_dialog_cleanup(struct analyzer_sip_call_dialog *d) {

	// We need to acquire this lock first in case we need to cleanup the call
	pom_rwlock_wlock(&analyzer_sip_calls_lock);

	struct analyzer_sip_call *call = d->call;

	// Make sure notbody else is using this call
	pom_mutex_lock(&call->lock);

	if (!d->to_tag) {
		debug_sip("Cleaning up half dialog for call %s : from_tag %s, branch %s", call->call_id, d->from_tag, d->branch);
	} else {
		debug_sip("Cleaning up full dialog for call %s : from_tag %s, to_tag %s", call->call_id, d->from_tag, d->to_tag);
	}

	conntrack_timer_cleanup(d->t);
	free(d->from_tag);
	free(d->branch);

	if (d->to_tag)
		free(d->to_tag);

	// Remove the dialog from the call
	if (d->prev)
		d->prev->next = d->next;
	else
		call->dialogs = d->next;

	if (d->next)
		d->next->prev = d->prev;

	free(d);

	int cleanup = 0;

	if (!call->dialogs && call->state != analyzer_sip_call_state_connected) {
		cleanup = 1;
		HASH_DEL(analyzer_sip_calls, call);
	}
	pom_mutex_unlock(&call->lock);
	pom_rwlock_unlock(&analyzer_sip_calls_lock);

	if (cleanup)
		analyzer_sip_call_cleanup(call);

	return POM_OK;
}

static int analyzer_sip_sdp_open(void *obj, void **priv, struct pload *pload) {

	struct analyzer *analyzer = obj;
	struct analyzer_sip_priv *apriv = analyzer->priv;

	struct event *evt = pload_get_related_event(pload);
	if (!evt)
		return PLOAD_OPEN_STOP;

	struct event_reg *evt_reg = event_get_reg(evt);
	if (evt_reg != apriv->evt_sip_req && evt_reg != apriv->evt_sip_rsp)
		return PLOAD_OPEN_STOP;

	struct conntrack_entry *ce = event_get_conntrack(evt);
	if (!ce)
		return PLOAD_OPEN_STOP;

	struct analyzer_sip_call *call = analyzer_sip_event_get_call(analyzer, evt);
	if (!call) // Error while finding the call
		return PLOAD_OPEN_ERR;


	if (!call->tel_call) {
		call->tel_call = telephony_call_alloc();
		if (!call->tel_call) {
			pom_mutex_unlock(&call->lock);
			return PLOAD_OPEN_ERR;
		}
	}

	// We don't need to keep the lock on the call
	pom_mutex_unlock(&call->lock);

	struct analyzer_sip_sdp_priv *sdp_priv = malloc(sizeof(struct analyzer_sip_sdp_priv));
	if (!sdp_priv) {
		pom_oom(sizeof(struct analyzer_sip_sdp_priv));
		return PLOAD_OPEN_ERR;
	}
	memset(sdp_priv, 0, sizeof(struct analyzer_sip_sdp_priv));

	sdp_priv->sdp = telephony_sdp_alloc();

	if (!sdp_priv->sdp) {
		free(sdp_priv);
		return PLOAD_OPEN_ERR;
	}

	sdp_priv->call = call;
	sdp_priv->ts = event_get_timestamp(evt);

	*priv = sdp_priv;

	return PLOAD_OPEN_CONTINUE;
}

static int analyzer_sip_sdp_write(void *obj, void *priv, void *data, size_t len) {

	struct analyzer_sip_sdp_priv *p = priv;
	return telephony_sdp_parse(p->sdp, data, len);
}


static int analyzer_sip_sdp_close(void *obj, void *priv) {

	struct analyzer_sip_sdp_priv *p = priv;

	if (telephony_sdp_parse_end(p->sdp) != POM_OK)
		return POM_ERR;

	if (telephony_sdp_add_expectations(p->call->tel_call, p->sdp, p->ts) != POM_OK)
		return POM_ERR;

	telephony_sdp_cleanup(p->sdp);

	free(p);

	return POM_OK;
}

static int analyzer_sip_dtmf_open(void *obj, void **priv, struct pload *pload) {

	struct analyzer *analyzer = obj;
	struct analyzer_sip_priv *apriv = analyzer->priv;

	struct event *evt = pload_get_related_event(pload);
	if (!evt)
		return PLOAD_OPEN_STOP;

	struct event_reg *evt_reg = event_get_reg(evt);
	if (evt_reg != apriv->evt_sip_req && evt_reg != apriv->evt_sip_rsp)
		return PLOAD_OPEN_STOP;

	struct event *dtmf_evt = event_alloc(apriv->evt_sip_dtmf);
	if (!dtmf_evt)
		return PLOAD_OPEN_ERR;

	struct data *evt_src_data = event_get_data(evt);
	struct data *evt_dst_data = event_get_data(dtmf_evt);
	analyzer_sip_event_common_data_copy(evt_dst_data, evt_src_data);

	struct data *pload_data = pload_get_data(pload);

	if (data_is_set(pload_data[analyzer_dtmf_pload_signal]))
		if (ptype_copy(evt_dst_data[analyzer_sip_dtmf_signal].value, pload_data[analyzer_dtmf_pload_signal].value) == POM_OK)
			data_set(evt_dst_data[analyzer_sip_dtmf_signal]);

	if (data_is_set(pload_data[analyzer_dtmf_pload_duration]))
		if (ptype_copy(evt_dst_data[analyzer_sip_dtmf_duration].value, pload_data[analyzer_dtmf_pload_duration].value) == POM_OK)
			data_set(evt_dst_data[analyzer_sip_dtmf_duration]);

	if (event_process(dtmf_evt, NULL, 0, event_get_timestamp(evt)) != POM_OK)
		return PLOAD_OPEN_ERR;

	return PLOAD_OPEN_STOP;
}
