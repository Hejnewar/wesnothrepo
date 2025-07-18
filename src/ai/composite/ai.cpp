/*
	Copyright (C) 2009 - 2025
	by Yurii Chernyi <terraninfo@terraninfo.net>
	Part of the Battle for Wesnoth Project https://www.wesnoth.org/

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY.

	See the COPYING file for more details.
*/

/**
 * Composite AI with turn sequence which is a vector of stages
 * @file
 */

#include "ai/composite/ai.hpp"
#include "ai/composite/aspect.hpp"
#include "ai/composite/engine.hpp"
#include "ai/composite/goal.hpp"
#include "ai/composite/property_handler.hpp"
#include "ai/composite/stage.hpp"
#include "ai/configuration.hpp"
#include "actions/attack.hpp"
#include "log.hpp"

#include <functional>

namespace ai {

static lg::log_domain log_ai_composite("ai/composite");
#define DBG_AI_COMPOSITE LOG_STREAM(debug, log_ai_composite)
#define LOG_AI_COMPOSITE LOG_STREAM(info, log_ai_composite)
#define ERR_AI_COMPOSITE LOG_STREAM(err, log_ai_composite)

// =======================================================================
// COMPOSITE AI
// =======================================================================
std::string ai_composite::describe_self() const
{
	return "[composite_ai]";
}

ai_composite::ai_composite( default_ai_context &context, const config &cfg)
	: cfg_(cfg),stages_(),recursion_counter_(context.get_recursion_count())
{
	init_default_ai_context_proxy(context);
}

void ai_composite::on_create()
{
	LOG_AI_COMPOSITE << "side "<< get_side() << " : "<<" created AI with id=["<<
		cfg_["id"]<<"]";

	// init the composite ai stages
	for (const config &cfg_element : cfg_.child_range("stage")) {
		add_stage(cfg_element);
	}

	register_vector_property(property_handlers(), "engine", get_engines(),
		[this](auto&&... args) { create_engine(args...); });

	register_vector_property(property_handlers(), "goal", get_goals(),
		[this](auto&&... args) { create_goal(args...); });

	register_vector_property(property_handlers(), "stage", stages_,
		[this](auto&&... args) { create_stage(args...); });

	register_aspect_property(property_handlers(), "aspect", get_aspects(),
		[this](auto&&... args) { replace_aspect(args...); });
}

void ai_composite::create_stage(std::vector<stage_ptr> &stages, const config &cfg)
{
	engine::parse_stage_from_config(*this,cfg,std::back_inserter(stages));
}

void ai_composite::create_goal(std::vector<goal_ptr> &goals, const config &cfg)
{
	engine::parse_goal_from_config(*this,cfg,std::back_inserter(goals));
}

void ai_composite::create_engine(std::vector<engine_ptr> &engines, const config &cfg)
{
	engine::parse_engine_from_config(*this,cfg,std::back_inserter(engines));
}

void ai_composite::replace_aspect(std::map<std::string,aspect_ptr> &aspects, const config &cfg, const std::string& id)
{
	std::vector<aspect_ptr> temp_aspects;
	engine::parse_aspect_from_config(*this,cfg,id,std::back_inserter(temp_aspects));
	aspects[id] = temp_aspects.back();
}

ai_composite::~ai_composite()
{
}

bool ai_composite::add_stage(const config &cfg)
{
	std::vector< stage_ptr > stages;
	create_stage(stages,cfg);
	int j=0;
	for (stage_ptr b : stages) {
		stages_.push_back(b);
		j++;
	}
	return (j>0);
}

bool ai_composite::add_goal(const config &cfg)
{
	std::vector< goal_ptr > goals;
	create_goal(goals,cfg);
	int j=0;
	for (goal_ptr b : goals) {
		get_goals().push_back(b);
		j++;
	}
	return (j>0);
}

void ai_composite::play_turn(){
	for (stage_ptr &s : stages_) {
		s->play_stage();
	}
}

std::string ai_composite::get_id() const
{
	return cfg_["id"];
}


std::string ai_composite::get_name() const
{
	return cfg_["name"];
}

std::string ai_composite::get_engine() const
{
	return cfg_["engine"];
}

void ai_composite::new_turn()
{
	recalculate_move_maps();
	invalidate_defensive_position_cache();
	invalidate_keeps_cache();
	clear_additional_targets();
	unit_stats_cache().clear();
}

int ai_composite::get_recursion_count() const
{
	return recursion_counter_.get_count();
}

void ai_composite::switch_side(side_number side)
{
	set_side(side);
}

ai_context& ai_composite::get_ai_context()
{
	return *this;
}

config ai_composite::to_config() const
{
	config cfg;

	//serialize the composite ai stages
	for (const stage_ptr &s : stages_) {
		cfg.add_child("stage",s->to_config());
	}

	return cfg;
}

config ai_composite::preparse_cfg(ai_context& ctx, const config& cfg)
{
	config temp_cfg, parsed_cfg;
	temp_cfg.add_child("ai", cfg);
	configuration::parse_side_config(ctx.get_side(), temp_cfg, parsed_cfg);
	return parsed_cfg;
}

} //end of namespace ai
