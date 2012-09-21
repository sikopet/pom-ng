/*
 *  This file is part of pom-ng.
 *  Copyright (C) 2012 Guy Martin <gmsoft@tuxicoman.be>
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

#ifndef __ADDON_PLUGIN_H__
#define __ADDON_PLUGIN_H__

#include <pom-ng/addon.h>

#define ADDON_PLUGIN_METATABLE		"addon.plugin"
#define ADDON_PLUGIN_LIB		"plugin"

struct addon_plugin_param {
	char *name;
	struct ptype *value;

	struct addon_plugin_param *next;
};

struct addon_plugin {
	struct addon_plugin_reg *reg;
	struct addon_plugin_param *params;

	int open;

	void *priv;
};

int addon_plugin_lua_register(lua_State *L);

#endif