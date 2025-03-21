/*
	Copyright (C) 2014 - 2025
	by Chris Beck <render787@gmail.com>
	Part of the Battle for Wesnoth Project https://www.wesnoth.org/

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY.

	See the COPYING file for more details.
*/

#include "scripting/mapgen_lua_kernel.hpp"

#include "config.hpp"
#include "game_errors.hpp"
#include "log.hpp"
#include "scripting/lua_common.hpp"
#include "scripting/lua_pathfind_cost_calculator.hpp"
#include "scripting/lua_terrainfilter.hpp"
#include "scripting/lua_terrainmap.hpp"
#include "deprecation.hpp"
#include "game_version.hpp"

#include <string>
#include <functional>

#include "scripting/push_check.hpp"
#include "generators/default_map_generator_job.hpp"

static lg::log_domain log_mapgen("mapgen");
#define ERR_NG LOG_STREAM(err, log_mapgen)
#define LOG_NG LOG_STREAM(info, log_mapgen)
#define DBG_NG LOG_STREAM(debug, log_mapgen)



// Template which allows to push member functions to the lua kernel into lua as C functions, using a shim
typedef int (mapgen_lua_kernel::*member_callback)(lua_State *);

template <member_callback method>
int dispatch(lua_State *L) {
	return ((lua_kernel_base::get_lua_kernel<mapgen_lua_kernel>(L)).*method)(L);
}

/**
 * Returns a random number, same interface as math.random.
 */
static int intf_random(lua_State *L)
{
	std::mt19937& rng = lua_kernel_base::get_lua_kernel<mapgen_lua_kernel>(L).get_default_rng();
	if(lua_isnoneornil(L, 1)) {
		double r = double (rng());
		double r_max = double (rng.max());
		lua_push(L, r / (r_max + 1));
		return 1;
	}
	else {
		int32_t min;
		int32_t max;
		if(lua_isnumber(L, 2)) {
			min = lua_check<int32_t>(L, 1);
			max = lua_check<int32_t>(L, 2);
		}
		else {
			min = 1;
			max = lua_check<int32_t>(L, 1);
		}
		if(min > max) {
			return luaL_argerror(L, 1, "min > max");
		}
		lua_push(L, min + static_cast<int>(rng() % (max - min + 1)));
		return 1;
	}
}

/**
 * calls the default mapgenerator.
 */
static int intf_default_generate(lua_State *L)
{
	std::mt19937& rng = lua_kernel_base::get_lua_kernel<mapgen_lua_kernel>(L).get_default_rng();

	int width = luaL_checkinteger(L, 1);
	int height = luaL_checkinteger(L, 2);

	config cfg = luaW_checkconfig(L, 3);

	generator_data arg;
	arg.width = width;
	arg.height = height;
	arg.nplayers = cfg["nplayers"].to_int(2);
	arg.nvillages = cfg["nvillages"].to_int(0);
	arg.iterations = cfg["iterations"].to_int(0);
	arg.hill_size = cfg["hill_size"].to_int(0);
	arg.castle_size = cfg["castle_size"].to_int(0);
	arg.island_size = cfg["island_size"].to_int(0);
	arg.island_off_center = cfg["island_off_center"].to_int(0);
	arg.max_lakes = cfg["max_lakes"].to_int(0);
	arg.link_castles = cfg["link_castles"].to_bool();
	arg.show_labels = cfg["show_labels"].to_bool(0);

	uint32_t seed = cfg["seed"].to_int(0);
	if(!cfg.has_attribute("seed")) {
		seed = rng();
	}

	default_map_generator_job job(seed);
	std::string res = job.default_generate_map(arg, nullptr, cfg);

	lua_push(L, res);
	return 1;
}

/**
 * calls the default mapgenerator.
 */
static int intf_default_generate_height_map(lua_State *L)
{
	std::mt19937& rng = lua_kernel_base::get_lua_kernel<mapgen_lua_kernel>(L).get_default_rng();

	int width = luaL_checkinteger(L, 1);
	int height = luaL_checkinteger(L, 2);

	config cfg = luaW_checkconfig(L, 3);

	if(!cfg.has_attribute("location_set")) {
		deprecated_message("generate_height_map(..., {location_set=false})", DEP_LEVEL::PREEMPTIVE, "1.17", "The default value of this option will be changed to true in 1.17.");
	}

	int iterations = cfg["iterations"].to_int(1);
	int hill_size = cfg["hill_size"].to_int(1);
	int island_size = cfg["island_size"].to_int(width/2);
	int center_x = cfg["center_x"].to_int(width/2);
	int center_y = cfg["center_y"].to_int(height/2);
	bool flip_layout = cfg["flip_format"].to_bool();
	bool as_locset = cfg["location_set"].to_bool(false);
	uint32_t seed = cfg["seed"].to_int(0);

	if(!cfg.has_attribute("seed")) {
		seed = rng();
	}
	default_map_generator_job job(seed);
	default_map_generator_job::height_map res = job.generate_height_map(width, height, iterations, hill_size, island_size, center_x, center_y);
	lua_createtable (L, width * height, 0);
	assert(int(res.size()) == width);
	assert((width == 0 || int(res[0].size()) == height));
	std::hash<map_location> loc_hash;
	for(int x = 0; x != width; ++x) {
		for(int y = 0; y != height; ++y) {
			int h = res[x][y];
			lua_pushinteger (L, h);
			if(as_locset) {
				map_location loc(flip_layout ? y : x, flip_layout ? x : y, wml_loc());
				lua_rawseti(L, -2, loc_hash(loc));
			} else {
				int i = flip_layout ? (y + x * height) : (x + y * width);
				lua_rawseti(L, -2, i);
			}
		}
	}
	return 1;
}
/**
 * Finds a path between two locations.
 * - Args 1: source location.
 * - Args 2: destination.
 * - Arg 3: cost function
 * - Args 4,5 size of map.
 * - Arg 6 include border.
 * OR
 * - Arg 3: options table containing calculate, width, height, (optional) include_borders
 * - Ret 1: array of pairs containing path steps.
 * - Ret 2: path cost.
 */
static int intf_find_path(lua_State *L)
{
	int arg = 1;
	map_location src = luaW_checklocation(L, 1), dst = luaW_checklocation(L, 2);
	if(lua_isfunction(L, arg)) {
		const char *msg = lua_pushfstring(L, "%s expected, got %s", lua_typename(L, LUA_TFUNCTION), luaL_typename(L, 3));
		return luaL_argerror(L, 3, msg);
	}
	utils::optional<lua_pathfind_cost_calculator> calc;
	int width, height;
	bool border = false;
	if(lua_istable(L, 3)) {
		if(luaW_tableget(L, 3, "calculate")) {
			calc = lua_pathfind_cost_calculator(L, lua_gettop(L));
		} else {
			return luaL_argerror(L, 3, "missing key: calculate");
		}
		if(!luaW_tableget(L, 3, "width")) {
			width = luaL_checkinteger(L, -1);
		} else {
			return luaL_argerror(L, 3, "missing key: width");
		}
		if(!luaW_tableget(L, 3, "height")) {
			height = luaL_checkinteger(L, -1);
		} else {
			return luaL_argerror(L, 3, "missing key: height");
		}
		if(!luaW_tableget(L, 3, "include_borders")) {
			border = luaW_toboolean(L, -1);
		}
	} else {
		calc = lua_pathfind_cost_calculator(L, 3);
		width = luaL_checkinteger(L, 4);
		height = luaL_checkinteger(L, 5);
		if(lua_isboolean(L, 6)) {
			border = luaW_toboolean(L, 6);
		}
	}
	pathfind::plain_route res = pathfind::a_star_search(src, dst, 10000, *calc, width, height, nullptr, border);

	int nb = res.steps.size();
	lua_createtable(L, nb, 0);
	for (int i = 0; i < nb; ++i)
	{
		lua_createtable(L, 2, 0);
		lua_pushinteger(L, res.steps[i].wml_x());
		lua_rawseti(L, -2, 1);
		lua_pushinteger(L, res.steps[i].wml_y());
		lua_rawseti(L, -2, 2);
		lua_rawseti(L, -2, i + 1);
	}
	lua_pushinteger(L, res.move_cost);

	return 2;
}


mapgen_lua_kernel::mapgen_lua_kernel(const config* vars)
	: lua_kernel_base()
	, random_seed_()
	, default_rng_()
	, vars_(vars)
{
	lua_State *L = mState;

	// Overwrite mathx.random. This guarantees that the mapgen_lua_kernel version
	// of mathx.random overrides the lua_kernel_base version.
	lua_getglobal(L, "mathx");
	lua_pushcfunction(L, &intf_random);
	lua_setfield(L, -2, "random");

	lua_settop(L, 0);

	static luaL_Reg const map_callbacks[] {
		// Map methods
		{ "find",                &intf_mg_get_locations            },
		{ "find_in_radius",      &intf_mg_get_tiles_radius         },
		// Static functions
		{ "filter",              &intf_terrainfilter_create        },
		{ "create",              &intf_terrainmap_create           },
		{ "generate_height_map", &intf_default_generate_height_map },
		{ "generate",            &intf_default_generate            },
		{ nullptr, nullptr }
	};

	luaW_getglobal(L, "wesnoth", "map");
	assert(lua_istable(L,-1));
	luaL_setfuncs(L, map_callbacks, 0);
	lua_pop(L, 1);
	assert(lua_gettop(L) == 0);

	// Create the paths module
	cmd_log_ << "Adding paths module...\n";
	static luaL_Reg const path_callbacks[] {
		{ "find_path",                 &intf_find_path                          },
		{ nullptr, nullptr }
	};
	lua_getglobal(L, "wesnoth");
	lua_newtable(L);
	luaL_setfuncs(L, path_callbacks, 0);
	lua_setfield(L, -2, "paths");
	lua_pop(L, 1);

	// Add functions to the WML module
	lua_getglobal(L, "wml");
	static luaL_Reg const wml_callbacks[] {
		{"tovconfig", &lua_common::intf_tovconfig},
		// These aren't actually part of the API - they're used internally by the variable metatable.
		{ "get_variable", &dispatch<&mapgen_lua_kernel::intf_get_variable>},
		{ "get_all_vars", &dispatch<&mapgen_lua_kernel::intf_get_all_vars>},
		{ nullptr, nullptr }
	};
	luaL_setfuncs(L, wml_callbacks, 0);
	lua_pop(L, 1);

	cmd_log_ << lua_terrainmap::register_metatables(L);
	cmd_log_ << lua_terrainfilter::register_metatables(L);
}

void mapgen_lua_kernel::run_generator(const char * prog, const config & generator)
{
	load_string(prog, "", std::bind(&lua_kernel_base::throw_exception, this, std::placeholders::_1, std::placeholders::_2));
	luaW_pushconfig(mState, generator);
	protected_call(1, 1, std::bind(&lua_kernel_base::throw_exception, this, std::placeholders::_1, std::placeholders::_2));
}

void mapgen_lua_kernel::user_config(const char * prog, const config & generator)
{
	run_generator(prog, generator);
}

int mapgen_lua_kernel::intf_get_variable(lua_State *L)
{
	char const *m = luaL_checkstring(L, 1);
	if(vars_) {
		variable_access_const v(m, *vars_);
		return luaW_pushvariable(L, v) ? 1 : 0;
	}
	return 0;
}

int mapgen_lua_kernel::intf_get_all_vars(lua_State *L) {
	luaW_pushconfig(L, vars_ ? *vars_ : config());
	return 1;
}

std::string mapgen_lua_kernel::create_map(const char * prog, const config & generator, utils::optional<uint32_t> seed) // throws game::lua_error
{
	random_seed_ = seed;
	default_rng_ = std::mt19937(get_random_seed());
	run_generator(prog, generator);

	if (!lua_isstring(mState,-1)) {
		std::string msg = "expected a string, found a ";
		msg += lua_typename(mState, lua_type(mState, -1));
		lua_pop(mState, 1);
		throw game::lua_error(msg.c_str(),"bad return value");
	}

	return lua_tostring(mState, -1);
}

config mapgen_lua_kernel::create_scenario(const char * prog, const config & generator, utils::optional<uint32_t> seed) // throws game::lua_error
{
	random_seed_ = seed;
	default_rng_ = std::mt19937(get_random_seed());
	run_generator(prog, generator);

	if (!lua_istable(mState, -1)) {
		std::string msg = "expected a config (table), found a ";
		msg += lua_typename(mState, lua_type(mState, -1));
		lua_pop(mState, 1);
		throw game::lua_error(msg.c_str(),"bad return value");
	}
	config result;
	if (!luaW_toconfig(mState, -1, result)) {
		std::string msg = "expected a config, but it is malformed ";
		lua_pop(mState, 1);
		throw game::lua_error(msg.c_str(),"bad return value");
	}
	return result;
}

uint32_t mapgen_lua_kernel::get_random_seed()
{
	if(random_seed_) {
		return (*random_seed_)++;
	}
	else {
		return lua_kernel_base::get_random_seed();
	}
}

std::mt19937& mapgen_lua_kernel::get_default_rng()
{
	if(!default_rng_) {
		default_rng_ = std::mt19937(get_random_seed());
	}
	return *default_rng_;
}
