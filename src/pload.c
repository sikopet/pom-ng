/*
 *  This file is part of pom-ng.
 *  Copyright (C) 2011-2015 Guy Martin <gmsoft@tuxicoman.be>
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


#include "config.h"
#include "pload.h"
#include "registry.h"
#include "core.h"
#include "filter.h"
#include <pom-ng/resource.h>
#include <pom-ng/ptype_string.h>
#include <pom-ng/ptype_uint32.h>

#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>

#ifdef HAVE_LIBMAGIC
#include <magic.h>

// libmagic isn't thread safe so we open a cookie per thread
static __thread magic_t magic_cookie = NULL;

// We require at least 64 bytes to do an analysis with libmagic
#define PLOAD_BUFFER_MAGIC_MIN_SIZE 64

#endif


static struct registry_class *pload_registry_class = NULL;
static struct ptype *pload_store_path = NULL;
static struct ptype *pload_store_mmap_block_size = NULL;
static size_t pload_page_size = 0;

static struct pload_listener_reg *pload_listeners = NULL;

static struct pload_type *pload_types = NULL;
static struct pload_mime_type *pload_mime_types_hash = NULL, *pload_mime_types_head = NULL;

static char* pload_class_names[]  = {
	"other",
	"application",
	"audio",
	"image",
	"video",
	"document",
	NULL

};

static struct datavalue_template payload_types_data_template[] = {
	{ .name = "name", .type = "string" },
	{ .name = "description", .type = "string" },
	{ .name = "extension", .type = "string" },
	{ .name = "class", .type = "string" },
	{ 0 }
};

static struct datavalue_template mime_types_data_template[] = {
	{ .name = "name", .type = "string" },
	{ .name = "mime", .type = "string" },
	{ 0 }
};

static struct resource_template pload_types_resource_template[] = {
	{ "payload_types", payload_types_data_template },
	{ "mime_types", mime_types_data_template },
	{ 0 }
};

static char *pload_decoders_noop[] = {
	"7bit",
	"8bit",
	"binary",
	NULL
};

int pload_init() {

	pload_store_path = ptype_alloc("string");
	pload_store_mmap_block_size = ptype_alloc_unit("uint32", "bytes");
	if (!pload_store_path || !pload_store_mmap_block_size)
		return POM_ERR;

	struct resource *r = NULL;
	struct resource_dataset *pload_types_ds = NULL, *mime_types_ds = NULL;
	
	
	struct registry_param *p = NULL;
	// Add the registry class
	pload_registry_class = registry_add_class(PLOAD_REGISTRY);
	if (!pload_registry_class)
		goto err;


	p = registry_new_param("store_path", "/tmp/", pload_store_path, "Path for temporary payload storage", REGISTRY_PARAM_FLAG_CLEANUP_VAL);
	if (registry_class_add_param(pload_registry_class, p) != POM_OK)
		goto err;

	p = registry_new_param("mmap_min_block_size", "16777216", pload_store_mmap_block_size, "Mimnum block size mapping for mmap", REGISTRY_PARAM_FLAG_CLEANUP_VAL);
	if (registry_class_add_param(pload_registry_class, p) != POM_OK)
		goto err;

	p = NULL;


	r = resource_open("payload_types", pload_types_resource_template);
	if (!r)
		goto err;

	pload_types_ds = resource_dataset_open(r, "payload_types");
	if (!pload_types_ds)
		goto err;

	while (1) {
		struct datavalue *v = NULL;
		int res = resource_dataset_read(pload_types_ds, &v);
		if (res == DATASET_QUERY_OK)
			break;

		if (res < 0) {
			pomlog(POMLOG_ERR "Error while reading payload_types resource");
			goto err;
		}

		struct pload_type *def = malloc(sizeof(struct pload_type));
		if (!def) {
			pom_oom(sizeof(struct pload_type));
			goto err;
		}
		memset(def, 0, sizeof(struct pload_type));

		char *cls_name = PTYPE_STRING_GETVAL(v[3].value);
		int cls_id;
		for (cls_id = 0; pload_class_names[cls_id] && strcmp(pload_class_names[cls_id], cls_name); cls_id++);
		if (!pload_class_names[cls_id]) {
			pomlog(POMLOG_WARN "Class %s does not exists", cls_name);
			free(def);
			continue;
		}

		def->name = strdup(PTYPE_STRING_GETVAL(v[0].value));
		def->description = strdup(PTYPE_STRING_GETVAL(v[1].value));
		def->extension = strdup(PTYPE_STRING_GETVAL(v[2].value));
		def->cls = cls_id;

		if (!def->name || !def->description || !def->extension) {
			free(def);
			pom_oom(strlen(PTYPE_STRING_GETVAL(v[0].value)));
			goto err;
		}

		def->reg_instance = registry_add_instance(pload_registry_class, def->name);
		if (!def->reg_instance) {
			free(def);
			goto err;
		}

		struct ptype *param = ptype_alloc("string");
		if (!param) {
			free(def);
			goto err;
		}
		p = registry_new_param("class", PTYPE_STRING_GETVAL(v[3].value), param, "Payload class", REGISTRY_PARAM_FLAG_CLEANUP_VAL | REGISTRY_PARAM_FLAG_IMMUTABLE);
		if (registry_instance_add_param(def->reg_instance, p) != POM_OK) {
			free(def);
			goto err;
		}

		param = ptype_alloc("string");
		if (!param) {
			free(def);
			goto err;
		}
		p = registry_new_param("description", def->description, param, "Payload description", REGISTRY_PARAM_FLAG_CLEANUP_VAL | REGISTRY_PARAM_FLAG_IMMUTABLE);
		if (registry_instance_add_param(def->reg_instance, p) != POM_OK) {
			free(def);
			goto err;
		}

		param = ptype_alloc("string");
		if (!param) {
			free(def);
			goto err;
		}
		p = registry_new_param("extension", def->extension, param, "Payload extension", REGISTRY_PARAM_FLAG_CLEANUP_VAL | REGISTRY_PARAM_FLAG_IMMUTABLE);
		if (registry_instance_add_param(def->reg_instance, p) != POM_OK) {
			free(def);
			goto err;
		}
		p = NULL;

		def->perf_analyzed = registry_instance_add_perf(def->reg_instance, "analyzed", registry_perf_type_counter, "Number of payload analyzed", "ploads");

		if (!def->perf_analyzed) {
			free(def);
			goto err;
		}

		// Add the payload with its name
		HASH_ADD_KEYPTR(hh, pload_types, def->name, strlen(def->name), def);

		pomlog(POMLOG_DEBUG "Registered payload type %s : class %s, extension .%s", def->name, pload_class_names[def->cls], def->extension);

	}

	resource_dataset_close(pload_types_ds);
	pload_types_ds = NULL;

	mime_types_ds = resource_dataset_open(r, "mime_types");
	if (!mime_types_ds)
		goto err;

	while (1) {
		struct datavalue *v = NULL;
		int res = resource_dataset_read(mime_types_ds, &v);
		if (res == DATASET_QUERY_OK)
			break;

		if (res < 0) {
			pomlog(POMLOG_ERR "Error while reading mime_types resource");
			goto err;
		}

		char *name = PTYPE_STRING_GETVAL(v[0].value);

		struct pload_type *def;
		HASH_FIND(hh, pload_types, name, strlen(name), def);
		if (!def) {
			pomlog(POMLOG_WARN "Definition %s not known", name);
			continue;
		}
		
		struct pload_mime_type *pmt = malloc(sizeof(struct pload_mime_type));
		if (!pmt) {
			pom_oom(sizeof(struct pload_mime_type));
			goto err;
		}
		pmt = memset(pmt, 0, sizeof(struct pload_mime_type));
		pmt->type = def;
		pmt->name = strdup(PTYPE_STRING_GETVAL(v[1].value));

		if (!pmt->name) {
			free(pmt);
			pom_oom(strlen(PTYPE_STRING_GETVAL(v[1].value)) + 1);
			goto err;
		}

		// Add the payload with the associated mime_type
		HASH_ADD_KEYPTR(hh, pload_mime_types_hash, pmt->name, strlen(pmt->name), pmt);

		pmt->next = pload_mime_types_head;
		pload_mime_types_head = pmt;

		pomlog(POMLOG_DEBUG "Mime type %s registered as %s", pmt->name, name);

	}

	resource_dataset_close(mime_types_ds);
	resource_close(r);

	// Get the page size
	pload_page_size  = sysconf(_SC_PAGESIZE);
	if (!pload_page_size)
		pload_page_size = 4096;

	return POM_OK;

err:

	if (p)
		registry_cleanup_param(p);

	if (mime_types_ds)
		resource_dataset_close(mime_types_ds);

	if (pload_types_ds)
		resource_dataset_close(pload_types_ds);

	if (r)
		resource_close(r);
	
	pload_cleanup();

	return POM_ERR;
}

void pload_thread_cleanup() {

#ifdef HAVE_LIBMAGIC
	if (magic_cookie)
		magic_close(magic_cookie);
#endif

}

void pload_cleanup() {

	struct pload_type *cur_type, *tmp;
	HASH_ITER(hh, pload_types, cur_type, tmp) {
		HASH_DELETE(hh, pload_types, cur_type);

		if (cur_type->name)
			free(cur_type->name);

		if (cur_type->extension)
			free(cur_type->extension);

		if (cur_type->description)
			free(cur_type->description);

		free(cur_type);
	}
	HASH_CLEAR(hh, pload_mime_types_hash);

	while (pload_mime_types_head) {
		struct pload_mime_type *tmp = pload_mime_types_head;
		pload_mime_types_head = tmp->next;
		free(tmp->name);
		free(tmp);
	}


	if (pload_registry_class)
		registry_remove_class(pload_registry_class);
}

#ifdef HAVE_LIBMAGIC
int pload_magic_open() {
	if (magic_cookie) {
		pomlog(POMLOG_ERR "Magic cookie already opened");
		return POM_ERR;
	}

	magic_cookie = magic_open(MAGIC_MIME);
	if (!magic_cookie) {
		pomlog(POMLOG_ERR "Error while allocating the magic cookie");
		return POM_ERR;
	}

	if (magic_load(magic_cookie, NULL)) {
		pomlog(POMLOG_ERR "Error while loading the magic database : %s", magic_error(magic_cookie));
		return POM_ERR;
	}

	return POM_OK;
}
#endif


struct pload *pload_alloc(struct event *rel_event, int flags) {

	if (!rel_event)
		return NULL;

	struct pload *pload = malloc(sizeof(struct pload));
	if (!pload) {
		pom_oom(sizeof(struct pload));
		return NULL;
	}
	memset(pload, 0, sizeof(struct pload));

	pload->rel_event = rel_event;
	pload->flags = flags | PLOAD_FLAG_NEED_ANALYSIS;
	pload->refcount = 1;

	event_refcount_inc(rel_event);

	return pload;

}

int pload_end(struct pload *pload) {

	while (pload->listeners) {
		struct pload_listener *tmp = pload->listeners;
		pload->listeners = tmp->next;

		if (tmp->reg->close) {
			if (tmp->reg->close(tmp->reg->obj, tmp->priv) != POM_OK) {
				pomlog(POMLOG_WARN "Error while closing a payload listener");
			}
		}

		pom_mutex_lock(&tmp->reg->lock);
		struct pload_listener_ploads *plst;
		HASH_FIND(hh, tmp->reg->ploads, &pload, sizeof(pload), plst);
		if (!plst) {
			pom_mutex_unlock(&tmp->reg->lock);
			pomlog(POMLOG_ERR "Pload nost found in the listener pload list.");
			return POM_ERR;
		}
		HASH_DEL(tmp->reg->ploads, plst);
		pom_mutex_unlock(&tmp->reg->lock);

		free(plst);

		free(tmp);
	}

	if (pload->store)
		pload_store_end(pload->store);

	if (pload->decoder)
		decoder_cleanup(pload->decoder);
	
	if (pload->buf.data)
		free(pload->buf.data);

	if (pload->analyzer_priv && pload->type && pload->type->analyzer && pload->type->analyzer->cleanup)
		pload->type->analyzer->cleanup(pload, pload->analyzer_priv);

	pload_refcount_dec(pload);

	return POM_OK;
}


void pload_refcount_inc(struct pload *pload) {
	
	__sync_fetch_and_add(&pload->refcount, 1);
}

void pload_refcount_dec(struct pload *pload) {

	if (__sync_sub_and_fetch(&pload->refcount, 1))
		return;

	if (pload->store)
		pload_store_release(pload->store);

	if (pload->data)
		data_cleanup_table(pload->data, pload->type->analyzer->data_reg);

	if (pload->mime_type)
		mime_type_cleanup(pload->mime_type);

	if (pload->filename)
		free(pload->filename);

	event_refcount_dec(pload->rel_event);

	free(pload);
}

int pload_set_mime_type(struct pload *p, char *mime_type) {

	if (!mime_type)
		return POM_ERR;

	if (!(p->flags & PLOAD_FLAG_NEED_ANALYSIS))
		return POM_ERR;

	p->mime_type = mime_type_parse(mime_type);
	if (!p->mime_type)
		return POM_ERR;

	// Check for the name embedded in the Content-Type header if no filename is specified yet
	if (!p->filename) {

		char *filename = mime_type_get_param(p->mime_type, "name");
		if (filename) {
			char *slash = strrchr(filename, '/');
			if (slash)
				filename = slash + 1;
			if (strlen(filename)) {
				p->filename = strdup(filename);
				if (!p->filename) {
					pom_oom(strlen(filename));
					return POM_ERR;
				}
			}
		}
	}

	struct pload_mime_type *pmt = NULL;
	HASH_FIND(hh, pload_mime_types_hash, p->mime_type->name, strlen(p->mime_type->name), pmt);

	if (pmt)
		p->type = pmt->type;

	return POM_OK;
}

struct mime_type *pload_get_mime_type(struct pload *p) {
	return p->mime_type;
}

int pload_set_type(struct pload *p, char *type) {
	
	if (!type)
		return POM_ERR;
	
	if (!(p->flags & PLOAD_FLAG_NEED_ANALYSIS))
		return POM_ERR;

	HASH_FIND(hh, pload_types, type, strlen(type), p->type);

	return POM_OK;
}

int pload_set_encoding(struct pload *p, char *encoding) {

	if (!encoding)
		return POM_ERR;
	
	if (!(p->flags & PLOAD_FLAG_NEED_ANALYSIS) || p->buf.data_len)
		return POM_ERR;

	// Check if the encoding is a NO OP
	int i;
	for (i = 0; pload_decoders_noop[i] && strcasecmp(encoding, pload_decoders_noop[i]); i++);

	if (pload_decoders_noop[i])
		return POM_OK;

	p->decoder = decoder_alloc(encoding);
	if (!p->decoder) {
		p->flags |= PLOAD_FLAG_IS_ERR;
		return POM_ERR;
	}

	return POM_OK;
}

void pload_set_expected_size(struct pload *p, size_t size) {
	p->expected_size = size;
}

int pload_buffer_grow(struct pload *p, size_t min_size) {

	size_t new_size = p->buf.buf_size;

	new_size = min_size - (min_size % pload_page_size) + pload_page_size + p->buf.buf_size;

	void *new_buf = realloc(p->buf.data, new_size);

	if (!new_buf) {
		pom_oom(pload_page_size);
		p->flags |= PLOAD_FLAG_IS_ERR;
		return POM_ERR;
	}

	p->buf.data = new_buf;
	p->buf.buf_size = new_size;

	return POM_OK;
}

int pload_buffer_append(struct pload *p, void *data, size_t len) {

	if (p->decoder) {
		struct decoder *d = p->decoder;
		// First we need to decode the data provided

		size_t estimated_len = decoder_estimate_output_size(d, len);

		d->next_in = data;
		d->avail_in = len;

		if (p->buf.buf_size - p->buf.data_len < estimated_len) {
			if (pload_buffer_grow(p, estimated_len) != POM_OK)
				return POM_ERR;
		}

		d->avail_out = p->buf.buf_size - p->buf.data_len;

		while (1) {
			d->next_out = p->buf.data + p->buf.data_len;

			int res = decoder_decode(d);

			if (res == DEC_END) {
				break;
			} else if (res == DEC_ERR) {
				p->flags |= PLOAD_FLAG_IS_ERR;
				return POM_ERR;
			} else if (res == DEC_MORE) {
				p->buf.data_len = p->buf.buf_size - d->avail_out;
				if (pload_buffer_grow(p, len) != POM_OK)
					return POM_ERR;
				d->avail_out = p->buf.buf_size - p->buf.data_len;
			} else if (!d->avail_in) {
				// Nothing more to decode
				break;
			}
		}

		p->buf.data_len = p->buf.buf_size - d->avail_out;
	} else {
		// No need to decode this
		
		if (p->buf.buf_size < p->buf.data_len + len) {
			if (pload_buffer_grow(p, len) != POM_OK)
				return POM_ERR;
		}
		memcpy(p->buf.data + p->buf.data_len, data, len);
		p->buf.data_len += len;
	}

	return POM_OK;
}

int pload_store_make_space(struct pload *p, size_t min_size) {

	struct pload_store_map *write_map = p->store->write_map;

	if (min_size < write_map->map_size - write_map->off_cur)
		return POM_OK;

	// The requested size is bigger than what's available
	// Let's remap the whole thing to a new offset
	if (write_map->map && munmap(write_map->map, write_map->map_size)) {
		pomlog(POMLOG_ERR "Error while unmapping the memory area : %s", pom_strerror(errno));
		return POM_ERR;
	}

	// Calculate the closest offset we can use
	off_t offset = write_map->off_cur - (write_map->off_cur % pload_page_size);

	write_map->off_start += offset;
	write_map->off_cur -= offset;

	size_t block_size = *PTYPE_UINT32_GETVAL(pload_store_mmap_block_size);

	// Round up to the next page aligned size
	block_size += pload_page_size - (block_size % pload_page_size);

	// Make sure block size is big enough ...
	if (block_size - write_map->off_cur < min_size) {
		block_size = write_map->off_cur + min_size;
		block_size += pload_page_size - (block_size % pload_page_size);
	}


	if (ftruncate(p->store->fd, write_map->off_start + block_size)) {
		pomlog(POMLOG_ERR "Error while expanding file \"%s\" : %s", p->store->filename, pom_strerror(errno));
		return POM_ERR;
	}

	write_map->map = mmap(NULL, block_size, PROT_READ | PROT_WRITE, MAP_SHARED, p->store->fd, write_map->off_start);
	if (write_map->map == MAP_FAILED) {
		pomlog(POMLOG_ERR "Error while mapping the file \"%s\" : %s", p->store->filename, pom_strerror(errno));
		return POM_ERR;
	}

	write_map->map_size = block_size;

	return POM_OK;
}

int pload_store_append(struct pload *p, void *data, size_t len) {

	struct pload_store_map *write_map = p->store->write_map;

	if (p->decoder) {
		struct decoder *d = p->decoder;
		// First we need to decode the data provided

		size_t estimated_len = decoder_estimate_output_size(d, len);

		d->next_in = data;
		d->avail_in = len;

		if (write_map->map_size - write_map->off_cur < estimated_len) {
			if (pload_store_make_space(p, estimated_len) != POM_OK)
				return POM_ERR;
		}

		d->avail_out = write_map->map_size - write_map->off_cur;

		while (1) {
			d->next_out = write_map->map + write_map->off_cur;

			int res = decoder_decode(d);

			if (res == DEC_END) {
				break;
			} else if (res == DEC_ERR) {
				p->flags |= PLOAD_FLAG_IS_ERR;
				return POM_ERR;
			} else if (res == DEC_MORE) {
				write_map->off_cur = write_map->map_size - d->avail_out;
				if (pload_store_make_space(p, pload_page_size) != POM_OK)
					return POM_ERR;
				d->avail_out = write_map->map_size - write_map->off_cur;
			} else if (!d->avail_in) {
				// Nothing more to decode
				break;
			}
		}

		write_map->off_cur = write_map->map_size - d->avail_out;
	} else {
		// No need to decode this
	
		if (write_map->map_size < write_map->off_cur + len) {
			if (pload_store_make_space(p, len) != POM_OK)
				return POM_ERR;
		}
		memcpy(write_map->map + write_map->off_cur, data, len);
		write_map->off_cur += len;
	}

	pom_mutex_lock(&p->store->lock);
	p->store->file_size = write_map->off_start + write_map->off_cur;
	int res = pthread_cond_broadcast(&p->store->cond);
	if (res) {
		pomlog(POMLOG_ERR "Error while signaling pload store condition : %s", pom_strerror(res));
		abort();
	}
	pom_mutex_unlock(&p->store->lock);

	return POM_OK;
}
int pload_append(struct pload *p, void *data, size_t len) {

	if (p->flags & PLOAD_FLAG_IS_ERR)
		return POM_OK;


	// Allright, let's see what to do. There a multiple scenario

	// If the pload is open and a pload_store is attached, use that
	if ((p->flags & PLOAD_FLAG_OPENED) && p->store) {
		struct pload_store_map *write_map = p->store->write_map;
		// Remember where we left off
		off_t start_off = write_map->off_start + write_map->off_cur;
		if (pload_store_append(p, data, len) != POM_OK)
			return POM_OK;

		// Use the data from the map
		data = write_map->map + (start_off - write_map->off_start);
		if (data < write_map->map || data > write_map->map + write_map->map_size) {
			pomlog(POMLOG_ERR "Internal error, write_map calculation went wrong");
			abort();
		}
		len = (write_map->off_start + write_map->off_cur) - start_off;
	}

	// Second, has the payload been processed and if so, is anybody interested ?
	if ((p->flags & PLOAD_FLAG_OPENED) && !p->listeners) {
		// Nope nobody is
		return POM_OK;
	}

	// Allright, so we need to do something with it
	if (!(p->flags & PLOAD_FLAG_OPENED)) {
		// The pload is not yet open so there is no store possible yet
		if (p->buf.data_len || p->decoder) {
			if (pload_buffer_append(p, data, len) != POM_OK)
				return POM_OK;
			data = p->buf.data;
			len = p->buf.data_len;
		}
	} else if (!p->store && p->decoder) {
		// There is no store being used but we still need some space to decode the payload
		if (pload_buffer_append(p, data, len) != POM_OK)
			return POM_OK;
		data = p->buf.data;
		len = p->buf.data_len;
	}


#ifdef HAVE_LIBMAGIC
	if (p->flags & PLOAD_FLAG_NEED_MAGIC) {

		if (len < PLOAD_BUFFER_MAGIC_MIN_SIZE || (p->expected_size && p->expected_size < PLOAD_BUFFER_MAGIC_MIN_SIZE && len < p->expected_size)) {
			// Not enough data for magic, buffer stuff if needed
			if (!p->buf.data_len) // Stuff was not buffered
				return pload_buffer_append(p, data, len);
			return POM_OK;
		}

		if (!magic_cookie) {
			if (pload_magic_open() != POM_OK) {
				p->flags |= PLOAD_FLAG_IS_ERR;
				return POM_OK;
			}
		}

		char *magic_mime_type_name = (char*) magic_buffer(magic_cookie, data, len);

		if (!magic_mime_type_name) {
			pomlog(POMLOG_ERR "Error while proceeding with magic : %s", magic_error(magic_cookie));
			p->flags |= PLOAD_FLAG_IS_ERR;
			return POM_OK;
		}

		struct mime_type *magic_mime_type = mime_type_parse(magic_mime_type_name);

		if (!magic_mime_type) {
			p->flags |= PLOAD_FLAG_IS_ERR;
			return POM_OK;
		}

		if (p->mime_type) {
			if (!strcmp(magic_mime_type->name, p->mime_type->name)) {
				// Mime types are the same, cleanup the magic one
				mime_type_cleanup(magic_mime_type);
			} else if (!strcmp(magic_mime_type->name, "binary") || !strcmp(magic_mime_type->name, "application/octet-stream") || !strcmp(magic_mime_type->name, "text/plain")) {
				// Irrelevant mime types, keep the original one
				mime_type_cleanup(magic_mime_type);
			} else {
				// Replace the existing mime_type by the magic one
				mime_type_cleanup(p->mime_type);
				p->mime_type = magic_mime_type;
				struct pload_mime_type *pmt = NULL;
				HASH_FIND(hh, pload_mime_types_hash, p->mime_type->name, strlen(p->mime_type->name), pmt);
				if (pmt)
					p->type = pmt->type;
			}

		} else {
			// There is no mime_type assigned, use the magic one unconditionally
				p->mime_type = magic_mime_type;
				struct pload_mime_type *pmt = NULL;
				HASH_FIND(hh, pload_mime_types_hash, p->mime_type->name, strlen(p->mime_type->name), pmt);
				if (pmt)
					p->type = pmt->type;
		}

		// No need for additional magic !
		p->flags &= ~PLOAD_FLAG_NEED_MAGIC;
	}

#endif

	if ((p->flags & PLOAD_FLAG_NEED_ANALYSIS) && (!p->type || !p->type->analyzer)) {
		// No analyzer, remove the analysis needed flag
		p->flags &= ~PLOAD_FLAG_NEED_ANALYSIS;
	}

	if (p->flags & PLOAD_FLAG_NEED_ANALYSIS) {
		struct pload_analyzer *a = p->type->analyzer;

		if (!p->data && a->data_reg) {
			p->data = data_alloc_table(a->data_reg);
			if (!p->data) {
				p->flags |= PLOAD_FLAG_IS_ERR;
				return POM_ERR;
			}
		}
		struct pload_buffer pb = {
			.data = data,
			.data_len = len,
			.buf_size = len
		};

		int res = a->analyze(p, (p->buf.data ? &p->buf : &pb), a->priv);

		if (res != PLOAD_ANALYSIS_MORE) {
			if (a->cleanup) {
				a->cleanup(p, p->analyzer_priv);
				p->analyzer_priv = NULL;
			}
		}

		if (res == PLOAD_ANALYSIS_ERR) {
			// Something went wrong during the analysis
			p->flags |= PLOAD_FLAG_IS_ERR;
			return POM_OK;
		} else if (res == PLOAD_ANALYSIS_FAILED) {
			// Payload type wasn't recognized
			p->type = NULL;
			p->flags &= ~PLOAD_FLAG_NEED_ANALYSIS;
			if (p->data) {
				data_cleanup_table(p->data, a->data_reg);
				p->data = NULL;
			}

		} else if (res == PLOAD_ANALYSIS_OK) {
			// Analysis is done
			p->flags &= ~PLOAD_FLAG_NEED_ANALYSIS;

			registry_perf_inc(p->type->perf_analyzed, 1);

		} else if (res == PLOAD_ANALYSIS_MORE) {
			if (!p->buf.data_len) // The analyzer needs more data
				return pload_buffer_append(p, data, len);
			return POM_OK;
		}

	}

	
	// At this point the payload is ready to be sent to a listener

	if (!(p->flags & PLOAD_FLAG_OPENED)) {

		struct pload_listener_reg *listeners[2] = { pload_listeners, NULL };
		if (p->type)
			listeners[1] = p->type->listeners;

		int i;
		for (i = 0; i < 2; i++) {

			struct pload_listener_reg *reg;
			
			for (reg = listeners[i]; reg; reg = reg->next) {

				if (reg->filter && pload_filter_match(reg->filter, p) != FILTER_MATCH_YES)
					continue;

				void *pload_priv = NULL;
				int res = reg->open(reg->obj, &pload_priv, p);
				if (res == PLOAD_OPEN_ERR) {
					pomlog(POMLOG_ERR "One listener errored out when opening a payload");
					continue;
				} else if (res == PLOAD_OPEN_STOP) {
					// Nothing to do
					continue;
				}

				struct pload_listener *lst = malloc(sizeof(struct pload_listener));
				if (!lst) {
					pom_oom(sizeof(struct pload_listener));
					return POM_ERR;
				}
				memset(lst, 0, sizeof(struct pload_listener));

				struct pload_listener_ploads *plst = malloc(sizeof(struct pload_listener_ploads));
				if (!plst) {
					free(lst);
					pom_oom(sizeof(struct pload_listener_ploads));
					return POM_ERR;
				}
				memset(plst, 0, sizeof(struct pload_listener_ploads));

				lst->reg = reg;
				lst->priv = pload_priv;
			
				lst->next = p->listeners;
				if (lst->next)
					lst->next->prev = lst;
				p->listeners = lst;

				plst->p = p;
				pom_mutex_lock(&reg->lock);
				HASH_ADD(hh, reg->ploads, p, sizeof(p), plst);
				pom_mutex_unlock(&reg->lock);
			}
		}

		p->flags |= PLOAD_FLAG_OPENED;


		if (p->store) {
			
			if (!p->store->refcount) {
				// Some listener allocated the store but then released it
				free(p->store);
				p->store = NULL;
			} else {
		

				// Storage was requested
				if (pload_store_open(p->store) != POM_OK)
					return POM_ERR;

				if (pload_store_make_space(p, len) != POM_OK)
					return POM_ERR;

				struct pload_store_map *write_map = p->store->write_map;
				memcpy(write_map->map + write_map->off_cur, data, len);
				write_map->off_cur += len;
			}
		}

	}

	struct pload_listener *tmp = p->listeners;
	while (tmp) {
		
		if (tmp->reg->write(tmp->reg->obj, tmp->priv, data, len) != POM_OK) {
			pomlog(POMLOG_WARN "Error while writing to a pload listener");
			tmp->reg->close(tmp->reg->obj, tmp->priv);

			struct pload_listener *todel = tmp;
			tmp = tmp->next;

			if (todel->prev)
				todel->prev->next = todel->next;
			else
				p->listeners = todel->next;

			if (todel->next)
				todel->next->prev = todel->prev;

			free(todel);

		}
		tmp = tmp->next;
	}


	if (p->buf.data) {
		free(p->buf.data);
		p->buf.data = NULL;
		p->buf.data_len = 0;
		p->buf.buf_size = 0;
	}



	return POM_OK;
}

struct event *pload_get_related_event(struct pload *p) {
	return p->rel_event;
}

void pload_set_parent(struct pload* p, struct pload *parent) {
	p->parent = parent;
}

void pload_set_analyzer_priv(struct pload *p, void *priv) {
	p->analyzer_priv = priv;
}

void *pload_get_analyzer_priv(struct pload *p) {
	return p->analyzer_priv;
}

int pload_set_analyzer(char *type, struct pload_analyzer *analyzer_reg) {

	struct pload_type *pload_type = NULL;

	HASH_FIND(hh, pload_types, type, strlen(type), pload_type);
	
	if (!pload_type) {
		pomlog(POMLOG_ERR "Cannot register analyzer for pload type %s as it doesn't exists", type);
		return POM_ERR;
	}

	if (pload_type->analyzer) {
		pomlog(POMLOG_ERR "Pload type %s already has an analyzer register", type);
		return POM_ERR;
	}

	pload_type->analyzer = analyzer_reg;

	return POM_OK;
}

struct data *pload_get_data(struct pload *p) {
	return p->data;
}

struct data_reg *pload_get_data_reg(struct pload *p) {
	if (!p->type || !p->type->analyzer)
		return NULL;

	return p->type->analyzer->data_reg;
}

int pload_listen_start(void *obj, char *pload_type, struct filter *filter, int (*open) (void *obj, void **priv, struct pload *pload), int (*write) (void *obj, void *priv, void *data, size_t len), int (*close) (void *obj, void *priv)) {

	core_assert_is_paused();

	struct pload_listener_reg *reg = malloc(sizeof(struct pload_listener_reg));
	if (!reg) {
		filter_cleanup(filter);
		pom_oom(sizeof(struct pload_listener_reg));
		return POM_ERR;
	}
	memset(reg, 0, sizeof(struct pload_listener_reg));

	reg->filter = filter;
	reg->obj = obj;
	reg->open = open;
	reg->write = write;
	reg->close = close;

	int res = pthread_mutex_init(&reg->lock, NULL);

	if (res) {
		pomlog(POMLOG_ERR "Error while initializing pload listener lock : %s", pom_strerror(res));
		filter_cleanup(filter);
		free(reg);
		return POM_ERR;

	}

	struct pload_listener_reg **head = &pload_listeners;
	if (pload_type) {
		struct pload_type *def;
		HASH_FIND(hh, pload_types, pload_type, strlen(pload_type), def);
		if (!def) {
			filter_cleanup(filter);
			free(reg);
			pomlog(POMLOG_ERR "Cannot find payload type %s", pload_type);
			return POM_ERR;
		}

		head = &def->listeners;
	}


	reg->next = *head;
	if (reg->next)
		reg->next->prev = reg;

	*head = reg;


	return POM_OK;
}

int pload_listen_stop(void *obj, char *pload_type) {

	core_assert_is_paused();

	struct pload_listener_reg **head = &pload_listeners;
	if (pload_type) {
		struct pload_type *def;
		HASH_FIND(hh, pload_types, pload_type, strlen(pload_type), def);
		if (!def) {
			pomlog(POMLOG_ERR "Cannot find payload type %s", pload_type);
			return POM_ERR;
		}

		head = &def->listeners;
	} else {
		pload_type = "'any'";
	}

	struct pload_listener_reg *reg;

	for (reg = *head; reg && reg->obj != obj; reg = reg->next);

	if (!reg) {
		pomlog(POMLOG_ERR "Payload listener %x not found for payload type %s", obj, pload_type);
		return POM_ERR;
	}
	if (reg->next)
		reg->next->prev = reg->prev;
	if (reg->prev)
		reg->prev->next = reg->next;
	else
		*head = reg->next;

	struct pload_listener_ploads *cur_pload, *tmp_pload;
	HASH_ITER(hh, reg->ploads, cur_pload, tmp_pload) {
		HASH_DEL(reg->ploads, cur_pload);

		struct pload_listener *l;
		for (l = cur_pload->p->listeners; l && l->reg != reg; l = l->next);
		if (!l) {
			pomlog(POMLOG_ERR "Pload of a specific listener not found.");
			free(cur_pload);
			continue;
		}

		if (reg->close(reg->obj, l->priv)) {
			pomlog(POMLOG_WARN "Error while closing pload");
		}
		if (l->next)
			l->next->prev = l->prev;
		if (l->prev)
			l->prev->next = l->next;
		else
			cur_pload->p->listeners = l->next;

		free(l);

		free(cur_pload);
	}


	int res = pthread_mutex_destroy(&reg->lock);
	if (res)
		pomlog(POMLOG_WARN "Error while destroying pload_listener mutex : %s", pom_strerror(res));

	if (reg->filter)
		filter_cleanup(reg->filter);

	free(reg);

	return POM_OK;
}


int pload_set_filename(struct pload *p, char *filename) {

	if (p->filename)
		free(p->filename);

	char *slash = strrchr(filename, '/');
	if (slash)
		filename = slash + 1;

	if (!strlen(filename))
		return POM_OK;

	p->filename = strdup(filename);
	if (!p->filename) {
		pom_oom(strlen(filename) + 1);
		return POM_ERR;
	}
	return POM_OK;
}

char *pload_get_filename(struct pload *p) {
	return p->filename;
}


struct pload_type *pload_get_type(struct pload *p) {
	return p->type;
}

struct pload_store *pload_store_get(struct pload *pload) {

	if (pload->flags & PLOAD_FLAG_OPENED) {
		pomlog(POMLOG_ERR "Internal error, trying to get a store for a payload after it has been open.");
		return NULL;
	}

	// This function will be called by the listeners' open function
	// There is no need to protect this by a lock
	if (!pload->store) {
		pload->store = malloc(sizeof(struct pload_store));
		if (!pload->store) {
			pom_oom(sizeof(struct pload_store));
			return NULL;
		}
		memset(pload->store, 0, sizeof(struct pload_store));
		pload->store->fd = -1;
		
		int res = pthread_mutex_init(&pload->store->lock, NULL);
		if (res) {
			pomlog(POMLOG_ERR "Error while initializing the pload store mutex : %s", pom_strerror(res));
			abort();
		}

		res = pthread_cond_init(&pload->store->cond, NULL);
		if (res) {
			pomlog(POMLOG_ERR "Error while initializing the pload condition : %s", pom_strerror(res));
			abort();
		}

		pload->store->p = pload;
		pload_refcount_inc(pload);
	}

	pload_store_get_ref(pload->store);

	return pload->store;
}

int pload_store_open_file(struct pload_store *ps) {

	if (ps->fd != -1)
		return POM_OK;

	if (!ps->filename) {
		char *path = PTYPE_STRING_GETVAL(pload_store_path);
		char filename[28] = { 0 };
		snprintf(filename, 27, "pom-ng_%"PRIX64".tmp", (uint64_t)ps);

		ps->filename = malloc(strlen(path) + strlen(filename) + 1);
		if (!ps->filename)
			return POM_ERR;
		strcpy(ps->filename, path);
		strcat(ps->filename, filename);
	}

	ps->fd = pom_open(ps->filename, O_RDWR | O_CREAT, 0666);
	if (ps->fd == -1)
		return POM_ERR;

	return POM_OK;
}

int pload_store_open(struct pload_store *ps) {

	if (ps->write_map) {
		pomlog(POMLOG_ERR "Write map already exists and shouldn't !");
		return POM_ERR;
	}

	ps->write_map = malloc(sizeof(struct pload_store_map));
	if (!ps->write_map) {
		pom_oom(sizeof(struct pload_store_map));
		return POM_ERR;
	}
	memset(ps->write_map, 0, sizeof(struct pload_store_map));

	if (ps->fd == -1) {
		if (pload_store_open_file(ps) != POM_OK) {
			free(ps->write_map);
			return POM_ERR;
		}
	}

	pload_store_get_ref(ps);

	return POM_OK;
}

void pload_store_end(struct pload_store *ps) {

	if (ps->write_map)
		pload_store_map_cleanup(ps->write_map);

	pom_mutex_lock(&ps->lock);

	ps->write_map = NULL;
	ps->flags |= PLOAD_STORE_FLAG_COMPLETE;

	if (ps->fd != -1 && !ps->read_maps) {
		if (ftruncate(ps->fd, ps->file_size)) {
			pomlog(POMLOG_ERR "Error while shrinking file \"%s\" : %s", ps->filename, pom_strerror(errno));
		}

		if (close(ps->fd)) {
			pomlog(POMLOG_WARN "Error while closing file \"%s\" : %s", ps->filename, pom_strerror(errno));
		}
		ps->fd = -1;
	}

	int res = pthread_cond_broadcast(&ps->cond);
	if (res) {
		pomlog(POMLOG_ERR "Error while signaling pload store condition : %s", pom_strerror(res));
		abort();
	}
	pom_mutex_unlock(&ps->lock);

	pload_store_release(ps);

}

struct pload *pload_store_get_pload(struct pload_store *ps) {
	return ps->p;
}

void pload_store_get_ref(struct pload_store *ps) {
	__sync_add_and_fetch(&ps->refcount, 1);
}

void pload_store_release(struct pload_store *ps) {

	if (__sync_sub_and_fetch(&ps->refcount, 1))
		return;

	// Refcount is 0 !

	ps->p->store = NULL;
	pload_refcount_dec(ps->p);

	if (ps->fd != -1) {
		if (close(ps->fd)) {
			pomlog(POMLOG_WARN "Error while closing file \"%s\" : %s", ps->filename, pom_strerror(errno));
		}
	}

	if (ps->filename) {

		if (unlink(ps->filename)) {
			pomlog(POMLOG_WARN "Error while removing temporary file \"%s\" : %s", ps->filename);
		}
		
		free(ps->filename);
	}

	int res = pthread_mutex_destroy(&ps->lock);
	if (res) {
		pomlog(POMLOG_ERR "Error while destroying the pload_store lock : %s", pom_strerror(res));
		abort();
	}

	res = pthread_cond_destroy(&ps->cond);
	if (res) {
		pomlog(POMLOG_ERR "Error while destroying the pload_store cond : %s", pom_strerror(res));
		abort();
	}

	free(ps);
}

void pload_store_map_cleanup(struct pload_store_map *map) {

	if (map->map) {
		if (munmap(map->map, map->map_size)) {
			pomlog(POMLOG_ERR "Error while unmapping file : %s", pom_strerror(errno));
		}
	}

	free(map);
}

struct pload_store_map *pload_store_read_start(struct pload_store *ps) {

	struct pload_store_map *map = malloc(sizeof(struct pload_store_map));
	if (!map) {
		pom_oom(sizeof(struct pload_store_map));
		return NULL;
	}
	memset(map, 0, sizeof(struct pload_store_map));
	map->store = ps;
	
	pom_mutex_lock(&ps->lock);

	if (!ps->filename) {
		free(map);
		pomlog(POMLOG_ERR "Unable to read empty pload store");
		pom_mutex_unlock(&ps->lock);
		return NULL;
	}

	if (ps->fd == -1) {
		ps->fd = pom_open(ps->filename, O_RDWR | O_CREAT, 0666);
		if (ps->fd == -1) {
			pom_mutex_unlock(&ps->lock);
			free(map);
			return NULL;
		}
	}

	map->next = ps->read_maps;
	if (map->next)
		map->next->prev = map;
	ps->read_maps = map;

	pom_mutex_unlock(&ps->lock);

	return map;
}

ssize_t pload_store_read(struct pload_store_map *map, void **buff, size_t count) {

	struct pload_store *ps = map->store;

	pom_mutex_lock(&ps->lock);
	size_t file_remaining = map->store->file_size - map->off_start - map->off_cur;

	while (!file_remaining && !(ps->flags & PLOAD_STORE_FLAG_COMPLETE)) {
		int res = pthread_cond_wait(&ps->cond, &ps->lock);
		if (res) {
			pomlog(POMLOG_ERR "Error while waiting for pload_store condition : %s", pom_strerror(errno));
			abort();
		}
		file_remaining = map->store->file_size - map->off_start - map->off_cur;
	}
	pom_mutex_unlock(&ps->lock);

	if (!file_remaining)
		return 0; // EOF


	size_t remaining = map->map_size - map->off_cur;
	if (!remaining) {
		// Map the next block
		uint32_t block_size = *PTYPE_UINT32_GETVAL(pload_store_mmap_block_size);
		if (block_size > file_remaining)
			block_size = file_remaining - (file_remaining % pload_page_size) + pload_page_size;

		if (map->map && munmap(map->map, map->map_size)) {
			pomlog(POMLOG_ERR "Error while unmapping file \"%s\" : %s", map->store->filename, pom_strerror(errno));
			return -1;
		}

		map->off_start += map->map_size;
		map->off_cur = 0;

		map->map = mmap(NULL, block_size, PROT_READ, MAP_SHARED, map->store->fd, map->off_start);
		if (map->map == MAP_FAILED) {
			map->map = NULL;
			pomlog(POMLOG_ERR "Error while mapping file \"%s\" : %s", map->store->filename, pom_strerror(errno)); 
			return -1;
		}
		map->map_size = block_size;

		if (block_size > file_remaining)
			remaining = file_remaining;
		else
			remaining = block_size;
	}

	if (remaining > file_remaining)
		remaining = file_remaining;

	if (remaining > count)
		remaining = count;


	*buff = map->map + map->off_cur;
	map->off_cur += remaining;

	return remaining;
}

void pload_store_read_end(struct pload_store_map *map) {

	pom_mutex_lock(&map->store->lock);

	if (map->next)
		map->next->prev = map->prev;
	if (map->prev)
		map->prev->next = map->next;
	else
		map->store->read_maps = map->next;

	if (map->store->fd != -1 && !map->store->read_maps && !map->store->write_map) {
		if (close(map->store->fd)) {
			pomlog(POMLOG_WARN "Error while closing file \"%s\" : %s", map->store->filename, pom_strerror(errno));
		}
		map->store->fd = -1;
	}

	pom_mutex_unlock(&map->store->lock);

	pload_store_map_cleanup(map);
}

int pload_filter_prop_compile(struct filter *f, char *prop_str, struct filter_value *v) {

	struct pload_filter_prop *prop = malloc(sizeof(struct pload_filter_prop));
	if (!prop) {
		pom_oom(sizeof(struct pload_filter_prop));
		return POM_ERR;
	}
	memset(prop, 0, sizeof(struct pload_filter_prop));
	v->type = filter_value_type_prop;
	v->val.prop.priv = prop;


	if (!strncmp(prop_str, "data.", strlen("data."))) {
		prop_str += strlen("data.");
		v->type = filter_value_type_prop;
		prop->type = pload_filter_prop_pload_data;


	} else if (!strncmp(prop_str, "evt.", strlen("evt."))) {
		prop_str += strlen("evt.");

		if (!strncmp(prop_str, "data.", strlen("data."))) {
			prop_str += strlen("data.");
			prop->type = pload_filter_prop_evt_data;
		} else if (!strcmp(prop_str, "name")) {
			prop->type = pload_filter_prop_evt_name;
			v->val.prop.out_type = filter_value_type_string;
		} else if (!strcmp(prop_str, "source")) {
			prop->type = pload_filter_prop_evt_source;
			v->val.prop.out_type = filter_value_type_string;
		} else {
			pomlog(POMLOG_ERR "Invalid value for event filter");
		}
	} else {
		pomlog(POMLOG_ERR "Unknown property %s", prop_str);
		return POM_ERR;
	}

	if (prop->type == pload_filter_prop_pload_data || prop->type == pload_filter_prop_evt_data) {
		char *key = strchr(prop_str, '[');

		if (key) {
			*key = 0;
			key++;
			char *key_end = strchr(key, ']');
			if (!key_end) {
				free(prop);
				pomlog(POMLOG_ERR "Missing ] for data key in filter for item %s", prop_str);
				return POM_ERR;
			}
			prop->key = strdup(key);
			if (!prop->key) {
				free(prop);
				pom_oom(strlen(key) + 1);
				return POM_ERR;
			}
		}

		prop->field_name = strdup(prop_str);
		if (!prop->field_name) {
			if (prop->key)
				free(prop->key);
			free(prop);
			pom_oom(strlen(prop_str) + 1);
			return POM_ERR;
		}
	}

	return POM_OK;
}

int pload_filter_prop_get_val(struct filter_value *inval, struct filter_value *outval, void *obj) {


	struct pload *p = obj;
	struct pload_filter_prop *prop = inval->val.prop.priv;
	struct event *evt = p->rel_event;
	struct event_reg_info *evt_info = event_reg_get_info(event_get_reg(evt));

	if (prop->type == pload_filter_prop_evt_name) {
		outval->type = filter_value_type_string;
		outval->val.string = evt_info->name;
		return POM_OK;
	} else if (prop->type == pload_filter_prop_evt_source) {
		outval->type = filter_value_type_string;
		outval->val.string = evt_info->source_name;
		return POM_OK;
	}

	// We're left with pload_filter_prop_pload_data and pload_filter_prop_evt_data

	struct data *data = NULL;
	struct data_reg *dr = NULL;

	if (prop->type == pload_filter_prop_pload_data) {
		data = pload_get_data(p);
		dr = pload_get_data_reg(p);
	} else if (prop->type == pload_filter_prop_evt_data) {
		data = event_get_data(evt);

		struct event_reg_info *info = event_get_info(evt);
		dr = info->data_reg;
	} else {
		return POM_ERR;
	}

	if (!data || !dr)
		return POM_OK;

	int i;
	for (i = 0; i < dr->data_count && strcmp(dr->items[i].name, prop->field_name); i++);

	if (i >= dr->data_count)
		return POM_OK;

	struct ptype *value = NULL;

	struct data_item_reg *itm_reg = &dr->items[i];
	if (prop->key) {
		if (!(itm_reg->flags & DATA_REG_FLAG_LIST))
			// Specific item isn't a list
			return POM_OK;

		// Find the right entry
		struct data_item *itm;
		for (itm = data[i].items; itm && strcmp(itm->key, prop->key); itm = itm->next);
		if (!itm)
			return POM_OK;

		value = itm->value;

	} else if (itm_reg->flags & DATA_REG_FLAG_LIST) {
		// Key not provided
		return POM_OK;
	} else {
		value = data[i].value;
	}

	if (!value)
		return POM_OK;

	filter_ptype_to_value(outval, value);

	return POM_OK;
}

void pload_filter_prop_cleanup(void *prop) {

	struct pload_filter_prop *p = prop;
	if (p->field_name)
		free(p->field_name);
	if (p->key)
		free(p->key);

	free(p);
}


struct filter *pload_filter_compile(char *filter_expr) {

	struct filter *f = filter_alloc(pload_filter_prop_compile, NULL, pload_filter_prop_get_val, pload_filter_prop_cleanup);
	if (!f)
		return NULL;
	if (filter_compile(filter_expr, f) != POM_OK) {
		filter_cleanup(f);
		return NULL;
	}
	return f;

}

int pload_filter_match(struct filter *f, struct pload *p) {

	return filter_match(f, p);
}
