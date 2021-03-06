/*
 *  This file is part of pom-ng.
 *  Copyright (C) 2011-2014 Guy Martin <gmsoft@tuxicoman.be>
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

#include <pom-ng/ptype.h>
#include <pom-ng/core.h>
#include <pom-ng/conntrack.h>
#include <pom-ng/ptype_bool.h>
#include <pom-ng/ptype_uint16.h>

#include "proto_mpeg_ts.h"

#include <string.h>
#include <arpa/inet.h>
#include <stddef.h>
#include <docsis.h>
#include <errno.h>

int proto_mpeg_ts_init(struct proto *proto, struct registry_instance *i) {

	struct proto_mpeg_ts_priv *priv = malloc(sizeof(struct proto_mpeg_ts_priv));
	if (!priv) {
		pom_oom(sizeof(struct proto_mpeg_ts_priv));
		return POM_ERR;
	}
	memset(priv, 0, sizeof(struct proto_mpeg_ts_priv));

	proto_set_priv(proto, priv);

	priv->perf_missed_pkts = registry_instance_add_perf(i, "missed_pkts", registry_perf_type_counter, "Total number of missed MPEG-TS packets.", "pkts");

	struct registry_param *p = NULL;

	priv->param_mpeg_ts_stream_timeout = ptype_alloc_unit("uint16", "seconds");
	if (!priv->param_mpeg_ts_stream_timeout)
		goto err;

	p = registry_new_param("stream_timeout", "60", priv->param_mpeg_ts_stream_timeout, "Timeout for each MPEG PID", 0);
	if (proto_add_param(proto, p) != POM_OK)
		goto err;

	p = NULL;

	priv->proto_docsis = proto_get("docsis");
	priv->proto_mpeg_sect = proto_get("mpeg_sect");

	if (!priv->proto_docsis || !priv->proto_mpeg_sect)
		goto err;


	return POM_OK;

err:

	if (p)
		registry_cleanup_param(p);

	proto_mpeg_ts_cleanup(proto);
	
	return POM_ERR;

}

int proto_mpeg_ts_process(void *proto_priv, struct packet *p, struct proto_process_stack *stack, unsigned int stack_index) {

	// WARNING THIS CODE ASSUMES THAT PACKETS ARRIVE IN ORDER
	// This should be achieved by packet threads affinity on an input level
	// If MPEG packets are not the link layer, then care should be taken to 
	// send them in the right order. For example by reoderding RTP or TCP packets containing them

	struct proto_mpeg_ts_priv *ppriv = proto_priv;
	struct proto_process_stack *s = &stack[stack_index];
	struct proto_process_stack *s_next = &stack[stack_index + 1];
	unsigned char *buff = s->pload;

	uint16_t pid = ((buff[1] & 0x1F) << 8) | buff[2];
	unsigned char pusi = buff[1] & 0x40;

	PTYPE_UINT16_SETVAL(s->pkt_info->fields_value[proto_mpeg_ts_field_pid], pid);

	int hdr_len = 4;


	// Filter out NULL packets
	if (pid == MPEG_TS_NULL_PID) {
		s_next->proto = NULL;
		s_next->pload = s->pload + hdr_len;
		s_next->plen = s->plen - hdr_len;
		return PROTO_OK;
	}

	unsigned char afc = (buff[3] & 0x30) >> 4;
	unsigned char tsc = (buff[3] & 0xC0) >> 6;

	// Try to find out what type or payload we are dealing with

	if (conntrack_get(stack, stack_index) != POM_OK)
		return PROTO_ERR;
	
	if (!s->ce->priv) {
		s->ce->priv = malloc(sizeof(struct proto_mpeg_ts_conntrack_priv));
		if (!s->ce->priv) {
			conntrack_unlock(s->ce);
			pom_oom(sizeof(struct proto_mpeg_ts_conntrack_priv));
			return PROTO_ERR;
		}
		memset(s->ce->priv, 0, sizeof(struct proto_mpeg_ts_conntrack_priv));

	}

	struct proto_mpeg_ts_conntrack_priv *priv = s->ce->priv;

	// Find the MPEG stream from the right input
	
	unsigned int i;
	for (i = 0; i < priv->streams_array_size && priv->streams[i].input != p->input; i++);

	struct proto_mpeg_ts_stream *stream = NULL;
	if (i >= priv->streams_array_size) {
		// New stream
		
		// We need to have a PUSI to analyze and start recomposing the content
		if (!pusi) {
			conntrack_unlock(s->ce);
			return PROTO_OK;
		}

		// Create the new stream
		struct proto_mpeg_ts_stream *new_streams = realloc(priv->streams, sizeof(struct proto_mpeg_ts_stream) * (priv->streams_array_size + 1));
		if (!new_streams) {
			conntrack_unlock(s->ce);
			pom_oom(sizeof(struct proto_mpeg_ts_stream) * (priv->streams_array_size + 1));
			return PROTO_ERR;
		}
		priv->streams = new_streams;
		memset(&priv->streams[priv->streams_array_size], 0, sizeof(struct proto_mpeg_ts_stream));
		stream = &priv->streams[priv->streams_array_size];
		priv->streams_array_size++;

		stream->input = p->input;
		stream->ppriv = ppriv;

		stream->t = timer_alloc(stream, proto_mpeg_ts_stream_cleanup);
		if (!stream->t) {
			conntrack_unlock(s->ce);
			return PROTO_ERR;
		}
		stream->ce = s->ce;
		stream->last_seq = (buff[3] - 1) & 0xF;

		// Remove the conntrack timer if any
		conntrack_delayed_cleanup(s->ce, 0, p->ts);
		
		// Identify the stream,
		if (pid == MPEG_TS_DOCSIS_PID) {
			stream->type = proto_mpeg_stream_type_docsis;
		}


	} else {
		stream = &priv->streams[i];
	}

	conntrack_unlock(s->ce);

	// Check for missing packets
	unsigned int missed = 0;
	if (afc & 1) // only increment last_seq when AFC has payload
		stream->last_seq = (stream->last_seq + 1) & 0xF;
	while (stream->last_seq != (buff[3] & 0xF)) {
		stream->last_seq = (stream->last_seq + 1) & 0xF;
		missed++;
	}

	if (missed) {

		if (stream->multipart) {
			if (!stream->pkt_tot_len) {
				// The last packet was too short to know the size
				// Drop the multipart
				packet_multipart_cleanup(stream->multipart);
				stream->multipart = NULL;
			} else {
				size_t missed_len = missed * (MPEG_TS_LEN - 4);

				size_t remaining_len = stream->pkt_tot_len - stream->pkt_cur_len;

				if (missed_len > remaining_len)
					missed_len = remaining_len;

				// Create a packet with filled in bytes
				struct packet *pkt = packet_alloc();
				if (!pkt)
					return PROTO_ERR;

				if (packet_buffer_alloc(pkt, missed_len, 0) != POM_OK) {
					packet_release(pkt);
					return PROTO_ERR;
				}

				memset(pkt->buff, 0xFF, missed_len);

				if (packet_multipart_add_packet(stream->multipart, pkt, stream->pkt_cur_len, missed_len, 0) != POM_OK) {
					packet_multipart_cleanup(stream->multipart);
					stream->multipart = NULL;
					stream->pkt_cur_len = 0;
					stream->pkt_tot_len = 0;
					return PROTO_ERR;
				}

				stream->pkt_cur_len += missed_len;

				if (stream->pkt_tot_len >= stream->pkt_cur_len) {
					timer_dequeue(stream->t);

					// Process the multipart
					struct packet_multipart *m = stream->multipart;
					stream->multipart = NULL;
					stream->pkt_cur_len = 0;
					stream->pkt_tot_len = 0;
					if (packet_multipart_process(m, stack, stack_index + 1) == PROTO_ERR)
						return PROTO_ERR;
				}
			}
		}

		registry_perf_inc(ppriv->perf_missed_pkts, missed);
		pomlog(POMLOG_DEBUG "Missed %u MPEG packet(s) on input %s", missed, stream->input->name);
	}

	// Check the Adaptation Field Control
	if (afc & 0x2) {
		hdr_len += buff[4] + 1;
		if (hdr_len >= MPEG_TS_LEN)
			return PROTO_INVALID;
	}

	if (!(afc & 1) || tsc) {
		s_next->proto = NULL;
		s_next->pload = NULL;
		s_next->plen = 0;
		return PROTO_OK;
	}

	unsigned char *pload = s->pload + hdr_len;
	unsigned char pusi_ptr = pload[0];

	// Try to identify the stream if unknown
	if (stream->type == proto_mpeg_stream_type_unknown && pusi) {

		if (pload[0] == 0x0 && pload[1] == 0x0 && pload[2] == 0x1) {
			// PES packet. They have no pointer if PUSI is 1
			// Currently not handled
			stream->type = proto_mpeg_stream_type_pes;
		} else {
			// The last option is a SECT packet
			stream->type = proto_mpeg_stream_type_sect;
		}
	}

	// Filter out PES and unknown packets
	if (stream->type == proto_mpeg_stream_type_pes || stream->type == proto_mpeg_stream_type_unknown) {
		s_next->pload = s->pload + hdr_len;
		s_next->plen = s->plen - hdr_len;
		s_next->proto = NULL;
		return PROTO_OK;
	}

	// Check the validity of the pointer
	if (pusi) {
		if (pusi_ptr > 183)
			return PROTO_INVALID;
		
		// PUSI is the first byte of the payload
		pload++;
	}

	// Add the payload to the multipart if any
	if (stream->multipart) {

		if (!stream->pkt_tot_len) {
			// Last packet was too short to know the size
			if (stream->type == proto_mpeg_stream_type_docsis) {
				if (stream->multipart->head->len >= sizeof(struct docsis_hdr)) {
					pomlog(POMLOG_DEBUG "MPEG paket with invalid length : %u", stream->multipart->head->len);
					return PROTO_INVALID;
				}

				unsigned char tmp_buff[sizeof(struct docsis_hdr)];
				memcpy(tmp_buff, stream->multipart->head->pkt->buff + stream->multipart->head->pkt_buff_offset, stream->multipart->head->len);
				memcpy(tmp_buff + stream->multipart->head->len, pload, sizeof(struct docsis_hdr) - stream->multipart->head->len);

				struct docsis_hdr *tmp_hdr = (struct docsis_hdr*)tmp_buff;
				stream->pkt_tot_len = ntohs(tmp_hdr->len) + sizeof(struct docsis_hdr);

			} else if (stream->type == proto_mpeg_stream_type_sect) {
				switch (stream->multipart->head->len) {
					case 1:
						stream->pkt_tot_len = ((pload[0] & 0xF) << 8) | pload[1];
						break;
					case 2:
						stream->pkt_tot_len = ((((unsigned char*)stream->multipart->head->pkt->buff + stream->multipart->head->pkt_buff_offset)[1] & 0xF) << 8) | pload[0];
						break;
				}
				stream->pkt_tot_len += 3; // add the 3 headers bytes
				
			}
		
		}

	}

	// Get the right payload protocol
	struct proto *next_proto = NULL;
	switch (stream->type) {
		case proto_mpeg_stream_type_docsis:
			next_proto = ppriv->proto_docsis;
			break;
		case proto_mpeg_stream_type_sect:
			next_proto = ppriv->proto_mpeg_sect;
			break;
		default:
			next_proto = NULL;
			break;
	}

	// We have the begining of a new packets, there are some stuff to do ...
	unsigned int pos = hdr_len;
	if (pusi) {
		pos++;

	
		// If we already have some parts of a packet, process it
		if (stream->multipart) {

			size_t remaining_len = stream->pkt_tot_len - stream->pkt_cur_len;

			timer_dequeue(stream->t);

			struct packet_multipart *m = stream->multipart;
			stream->multipart = NULL;
			stream->pkt_tot_len = 0;
			
			if (pusi_ptr != remaining_len) {
				pomlog(POMLOG_DEBUG "Invalid tail length for %s packet : expected %u, got %hhu", (stream->type == proto_mpeg_stream_type_docsis ? "DOCSIS" : "SECT" ), remaining_len, pusi_ptr);
				packet_multipart_cleanup(m);
			} else {

				// Add the end of the previous packet
				if (packet_multipart_add_packet(m, p, stream->pkt_cur_len, pusi_ptr, hdr_len) != POM_OK)
					return PROTO_ERR;
				
				// Process the multipart once we're done with the MPEG packet
				if (packet_multipart_process(m, stack, stack_index + 1) == PROTO_ERR)
					return PROTO_ERR;
			}

			stream->pkt_cur_len = 0;

		}
		pos += pusi_ptr;

		if (pos >= MPEG_TS_LEN)
			// Nothing left to process
			return PROTO_STOP;

		while (1) {
			// Skip stuff bytes
			while (buff[pos] == 0xFF) {
				pos++;
				if (pos >= MPEG_TS_LEN)
					// Nothing left to process
					return PROTO_STOP;
			}
		
			if ( (stream->type == proto_mpeg_stream_type_docsis && (pos > (MPEG_TS_LEN - offsetof(struct docsis_hdr, hcs))))
				|| (stream->type == proto_mpeg_stream_type_sect && (pos > (MPEG_TS_LEN - 3)))) {
				// Cannot fetch the complete packet size, will do later
				stream->multipart = packet_multipart_alloc(next_proto, 0, 0);
				if (!stream->multipart)
					return PROTO_ERR;
				stream->pkt_tot_len = 0;
				stream->pkt_cur_len = 0;
				break;
			}

			// Check for self contained packets
			unsigned int pkt_len = 0;
			if (stream->type == proto_mpeg_stream_type_docsis) {
				struct docsis_hdr *docsis_hdr = (void*)buff + pos;
				memcpy(&pkt_len, &docsis_hdr->len, sizeof(docsis_hdr->len));
				pkt_len = ntohs(pkt_len) + sizeof(struct docsis_hdr);
			} else if (stream->type == proto_mpeg_stream_type_sect) {
				pkt_len = (((*(buff + pos + 1) & 0xF) << 8) | *(buff + pos + 2)) + 3;
			} else {
				pomlog(POMLOG_ERR "Internal error : Unhandled stream type");
				return PROTO_ERR;
			}
			if (pkt_len + pos > MPEG_TS_LEN) {
				stream->multipart = packet_multipart_alloc(next_proto, 0, 0);
				if (!stream->multipart)
					return PROTO_ERR;
				stream->pkt_tot_len = pkt_len;
				stream->pkt_cur_len = 0;

				break;
			}

			s_next->proto = next_proto;

#ifdef FIX_PACKET_ALIGNMENT
			char offset = pos & 3;
#else
			char offset = 0;
#endif
			if (offset) {
				struct packet_multipart *tmp = packet_multipart_alloc(s_next->proto, 0, 0);
				if (packet_multipart_add_packet(tmp, p, 0, pkt_len, pos) != POM_OK) {
					packet_multipart_cleanup(tmp);
					return PROTO_ERR;
				}

				if (packet_multipart_process(tmp, stack, stack_index + 1) != POM_OK)
					return PROTO_ERR;

			} else {

				// Process the packet
				s_next->pload = buff + pos;
				s_next->plen = pkt_len;
				int res = core_process_multi_packet(stack, stack_index + 1, p);
				if (res == PROTO_ERR)
					return PROTO_ERR;
			}
			pos += pkt_len;

			if (pos >= MPEG_TS_LEN)
				// Nothing left to process
				return PROTO_STOP;

		}

	} else if (!stream->multipart) {
		return PROTO_INVALID;
	}


	// Some leftover, add to multipart
	
	if (packet_multipart_add_packet(stream->multipart, p, stream->pkt_cur_len, MPEG_TS_LEN - pos, pos) != POM_OK) {	
		packet_multipart_cleanup(stream->multipart);
		stream->multipart = NULL;
		stream->pkt_cur_len = 0;
		stream->pkt_tot_len = 0;
		return PROTO_ERR;
	}

	stream->pkt_cur_len += MPEG_TS_LEN - pos;
	if (stream->pkt_tot_len && stream->pkt_cur_len >= stream->pkt_tot_len) {
		timer_dequeue(stream->t);
		// Process the multipart
		struct packet_multipart *m = stream->multipart;
		stream->multipart = NULL;
		stream->pkt_cur_len = 0;
		stream->pkt_tot_len = 0;
		if (packet_multipart_process(m, stack, stack_index + 1) == PROTO_ERR)
			return PROTO_ERR;
	}

	if (stream->multipart) {
		uint16_t *stream_timeout = PTYPE_UINT16_GETVAL(stream->ppriv->param_mpeg_ts_stream_timeout);
		timer_queue_now(stream->t, *stream_timeout, p->ts);
	}

	// No need to process further, we take care of that
	return PROTO_STOP;

}


int proto_mpeg_ts_stream_cleanup(void *priv, ptime now) {


	struct proto_mpeg_ts_stream *stream = priv;
	
	pom_mutex_lock(&stream->ce->lock);

	// Cleanup the stream stuff
	if (stream->multipart)
		packet_multipart_cleanup(stream->multipart);

	timer_cleanup(stream->t);
	
	// Remove it from the table
	struct proto_mpeg_ts_conntrack_priv *cpriv = stream->ce->priv;

	// Find out where it is in the table
	unsigned int i;
	for (i = 0; i < cpriv->streams_array_size && cpriv->streams[i].input != stream->input; i++);

	if (i >= cpriv->streams_array_size) {
		pomlog(POMLOG_ERR "Internal error, stream not found in the conntrack priv while cleaning it up");
		pom_mutex_unlock(&stream->ce->lock);
		return POM_ERR;
	}

	if (cpriv->streams_array_size == 1) {
		// Cleanup the conntrack in 10 seconds if no more packets
		conntrack_delayed_cleanup(stream->ce, 10, now);
	} else {
		size_t len = (cpriv->streams_array_size - i - 1) * sizeof(struct proto_mpeg_ts_stream);
		if (len)
			memmove(&cpriv->streams[i], &cpriv->streams[i+1], len);
	}

	cpriv->streams_array_size--;
	struct proto_mpeg_ts_stream *new_streams = realloc(cpriv->streams, sizeof(struct proto_mpeg_ts_stream) * cpriv->streams_array_size);

	if (cpriv->streams_array_size && !new_streams) {
		// Not really an issue as we lowered the size anyway
		pom_oom(sizeof(struct proto_mpeg_ts_stream) * cpriv->streams_array_size);
	} else {
		cpriv->streams = new_streams;
	}
	
	pom_mutex_unlock(&stream->ce->lock);

	return POM_OK;
}

int proto_mpeg_ts_conntrack_cleanup(void *ce_priv) {

	struct proto_mpeg_ts_conntrack_priv *priv = ce_priv;
	unsigned int i;
	for (i = 0; i < priv->streams_array_size; i++){ 
		if (priv->streams[i].multipart) 
			packet_multipart_cleanup(priv->streams[i].multipart);
		timer_cleanup(priv->streams[i].t);
	}
	if (priv->streams)
		free(priv->streams);

	free(priv);

	return POM_OK;
}

int proto_mpeg_ts_cleanup(void *proto_priv) {

	if (proto_priv) {
		struct proto_mpeg_ts_priv *priv = proto_priv;

		if (priv->param_mpeg_ts_stream_timeout)
			ptype_cleanup(priv->param_mpeg_ts_stream_timeout);

		free(priv);
	}

	return POM_OK;
}
