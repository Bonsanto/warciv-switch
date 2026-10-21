/***********************************************************************
 Freeciv - Copyright (C) 1996 - A Kjeldberg, L Gregersen, P Unold
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
***********************************************************************/

#ifdef HAVE_CONFIG_H
#include <fc_config.h>
#endif

#include <string.h>
#include <math.h> /* pow, ceil */

/* utility */
#include "log.h"
#include "mem.h"

/* common */
#include "city.h"
#include "effects.h"
#include "game.h"
#include "government.h"
#include "improvement.h"
#include "map.h"
#include "movement.h"
#include "research.h"
#include "tech.h"
#include "traderoutes.h"
#include "unit.h"
#include "unitlist.h"
#include "unittype.h"

/* common/aicore */
#include "pf_tools.h"

/* server */
#include "citytools.h"
#include "unittools.h"
#include "srv_log.h"

/* server/advisors */
#include "advbuilding.h"
#include "advchoice.h"
#include "advdata.h"
#include "infracache.h"

/* ai */
#include "aitraits.h"
#include "handicaps.h"

/* ai/default */
#include "daicity.h"
#include "daidata.h"
#include "daimilitary.h"
#include "daiplayer.h"
#include "daitech.h"
#include "daitools.h"

#include "daidomestic.h"

#define DAI_LARGEPOX_CAPITALIZATION_PROJECTED_MIN 95


/***********************************************************************//**
  Count the largepox city pipeline, including mobile founders and founders
  currently in production. Without this, several cities can all react to the
  same missing city and overshoot the compact largepox target together.
****************************************************************************/
static int dai_largepox_founder_pipeline(const struct player *pplayer)
{
  int founders = 0;

  unit_list_iterate(pplayer->units, punit) {
    /* Every population-cost founder is real capacity already paid for.
     * Ignoring founders temporarily building a road made the production
     * advisor replace them every retry cycle; representative Democracy
     * saves accumulated five to nine Settlers, consuming ten to eighteen
     * food per turn.  Existing founders can reconsider a city site when
     * their current task finishes, so count them all and never manufacture
     * duplicates merely because their mission is temporarily civilian. */
    if (unit_is_cityfounder(punit)) {
      founders++;
    }
  } unit_list_iterate_end;

  city_list_iterate(pplayer->cities, pcity) {
    if (VUT_UTYPE == pcity->production.kind
        && utype_is_cityfounder(pcity->production.value.utype)) {
      founders++;
    }
  } city_list_iterate_end;

  return founders;
}

/***********************************************************************//**
  Count only founders that have completed production.

  During the opening, production is deliberately speculative: every city
  accumulates shields for its next Settler, but those half-built units must
  not be treated as committed cities.  Once enough mobile founders exist,
  the ordinary production advisor can switch the surplus builds to caravans
  before they complete.
****************************************************************************/
static int dai_largepox_mobile_founders(const struct player *pplayer)
{
  int founders = 0;

  unit_list_iterate(pplayer->units, punit) {
    if (unit_is_cityfounder(punit)) {
      founders++;
    }
  } unit_list_iterate_end;

  return founders;
}

/***********************************************************************//**
  Count ferries already available or being built for the compact second
  expansion wave.  The generic founder advisor asks for a new ferry from
  every city when its best remaining site is overseas; on a twelve-city
  island that can turn the entire economy into Triremes in one turn.
****************************************************************************/
static int dai_largepox_ferry_pipeline(const struct player *pplayer)
{
  int ferries = 0;

  unit_list_iterate(pplayer->units, punit) {
    if (unit_has_type_role(punit, L_FERRYBOAT)) {
      ferries++;
    }
  } unit_list_iterate_end;

  city_list_iterate(pplayer->cities, pcity) {
    if (VUT_UTYPE == pcity->production.kind
        && utype_has_role(pcity->production.value.utype, L_FERRYBOAT)) {
      ferries++;
    }
  } city_list_iterate_end;

  return ferries;
}

/***********************************************************************//**
  True when an improvement belongs to the explicit largepox reinvestment
  chain.  Keep this in one place so production priority and crash buying
  cannot silently diverge.
****************************************************************************/
static bool dai_largepox_is_economic_building(
  const struct impr_type *building)
{
  static const char *const chain[] = {
    "Marketplace", "Bank", "Harbour", "Aqueduct",
    "Stock Exchange", "Library", "Sewer System", "University",
    "Factory", "Offshore Platform", "Supermarket", "Mass Transit"
  };

  for (size_t i = 0; i < ARRAY_SIZE(chain); i++) {
    if (0 == fc_strcasecmp(improvement_rule_name(building), chain[i])) {
      return TRUE;
    }
  }

  return FALSE;
}

/***********************************************************************//**
  Return the desired number of mobile plus in-production founders.

  Successful largepox openings do not serialize exactly the number of
  Settlers still missing.  Every opening city starts accumulating shields,
  then surplus builds are changed to caravans as soon as the last required
  mobile founders complete.  This speculative production is what produces
  the observed 3 -> 5 -> 9 -> 12 cadence.  The later 12 -> 15 coastal infill
  remains bounded to three founders so it cannot consume the mature economy.
****************************************************************************/
static int dai_largepox_desired_founder_pipeline(const struct player *pplayer,
                                                  int missing)
{
  int cities = city_list_size(pplayer->cities);

  if (missing <= 0) {
    return 0;
  }

  if (cities < 9) {
    /* The pipeline includes mobile founders, hence the extra missing term:
     * even while one Settler travels, all current cities may keep preparing
     * the next wave. */
    return cities + missing;
  }

  if (cities < 12) {
    /* At nine cities the last three sites are already known.  Keep only the
     * nearest-to-completion builds and convert the speculative remainder to
     * caravans, so the trade wave starts while those founders are moving. */
    return missing;
  }

  return MIN(3, missing);
}

/***********************************************************************//**
  Rank an in-production opening founder by readiness.

  A size-two city can actually finish its population-cost unit, so it ranks
  ahead of a size-one city even when the latter has more stored shields.
  Within that group prefer stored production, current shield output and food
  progress; the city id makes ties deterministic.
****************************************************************************/
static int dai_largepox_founder_readiness(const struct city *pcity)
{
  const struct unit_type *founder = pcity->production.value.utype;
  int ready = city_size_get(pcity) > founder->pop_cost ? 1 : 0;

  return ready * 1000000
         + pcity->shield_stock * 1000
         + MAX(0, pcity->surplus[O_SHIELD]) * 10
         + pcity->food_stock;
}

/***********************************************************************//**
  Return whether this speculative founder is one of the builds required to
  complete the opening target.
****************************************************************************/
static bool dai_largepox_founder_build_committed(const struct player *pplayer,
                                                  const struct city *pcity,
                                                  int slots)
{
  int score;
  int ahead = 0;

  if (slots <= 0
      || pcity->production.kind != VUT_UTYPE
      || !utype_is_cityfounder(pcity->production.value.utype)) {
    return FALSE;
  }

  score = dai_largepox_founder_readiness(pcity);
  city_list_iterate(pplayer->cities, other) {
    int other_score;

    if (other == pcity
        || other->production.kind != VUT_UTYPE
        || !utype_is_cityfounder(other->production.value.utype)) {
      continue;
    }

    other_score = dai_largepox_founder_readiness(other);
    if (other_score > score
        || (other_score == score && other->id < pcity->id)) {
      ahead++;
    }
  } city_list_iterate_end;

  return ahead < slots;
}

/***********************************************************************//**
  Count established routes plus trade units already paid for by this city.
  Those units are part of the route pipeline even while they are travelling.
****************************************************************************/
static int dai_largepox_routes_in_pipeline(const struct city *pcity)
{
  int routes = city_num_trade_routes(pcity);

  unit_list_iterate(pcity->units_supported, punit) {
    if (unit_can_do_action(punit, ACTION_TRADE_ROUTE)) {
      routes++;
    }
  } unit_list_iterate_end;

  return routes;
}

/***********************************************************************//**
  Read route coverage from the authoritative city state.

  The cached largepox snapshot is intentionally updated only once per AI
  phase.  Several routes can arrive later in that same phase, so using the
  snapshot for production keeps every city in caravan mode for one extra
  turn precisely when the route windfall should start Marketplace.
****************************************************************************/
static int dai_largepox_actual_route_coverage(const struct player *pplayer)
{
  int established = 0;
  int slots = 0;

  city_list_iterate(pplayer->cities, pcity) {
    established += city_num_trade_routes(pcity);
    slots += max_trade_routes(pcity);
  } city_list_iterate_end;

  return slots > 0 ? MIN(100, 100 * established / slots) : 100;
}

/***********************************************************************//**
  Estimate empire-wide route coverage after caravans already on the map
  reach their destinations.  An internal route fills one endpoint at its
  home and one at its destination.  Do not count units merely in production:
  switching those builds now would cancel the very caravan wave being
  projected.
****************************************************************************/
static int dai_largepox_projected_route_coverage(const struct player *pplayer)
{
  int projected = 0;
  int slots = 0;

  city_list_iterate(pplayer->cities, pcity) {
    projected += city_num_trade_routes(pcity);
    slots += max_trade_routes(pcity);
  } city_list_iterate_end;

  if (slots <= 0) {
    return 100;
  }

  unit_list_iterate(pplayer->units, punit) {
    if (unit_can_do_action(punit, ACTION_TRADE_ROUTE)) {
      projected += 2;
    }
  } unit_list_iterate_end;

  return MIN(100, 100 * projected / slots);
}

/***********************************************************************//**
  Count mobile and in-production trade units across the empire.  Production
  selection is applied city by city, so this aggregate count lets a single
  advisor pass request exactly the missing caravan wave instead of having
  every city independently request a boat or another caravan.
****************************************************************************/
static int dai_largepox_trade_unit_pipeline(const struct player *pplayer)
{
  int caravans = 0;

  unit_list_iterate(pplayer->units, punit) {
    if (unit_can_do_action(punit, ACTION_TRADE_ROUTE)) {
      caravans++;
    }
  } unit_list_iterate_end;

  city_list_iterate(pplayer->cities, pcity) {
    if (VUT_UTYPE == pcity->production.kind
        && utype_can_do_action(pcity->production.value.utype,
                               ACTION_TRADE_ROUTE)) {
      caravans++;
    }
  } city_list_iterate_end;

  return caravans;
}

/***********************************************************************//**
  Count dedicated terrain improvers. Population-cost founders also carry the
  Workers flag in classic rulesets, but they are scarce expansion capacity
  and must not satisfy the long-term worker target.
****************************************************************************/
static int dai_largepox_worker_pipeline(const struct player *pplayer,
                                        int *in_production)
{
  int workers = 0;
  int producing = 0;

  unit_list_iterate(pplayer->units, punit) {
    const struct unit_type *utype = unit_type_get(punit);

    if (utype_has_flag(utype, UTYF_WORKERS)
        && !utype_is_cityfounder(utype)) {
      workers++;
    }
  } unit_list_iterate_end;

  city_list_iterate(pplayer->cities, pcity) {
    if (VUT_UTYPE == pcity->production.kind
        && utype_has_flag(pcity->production.value.utype, UTYF_WORKERS)
        && !utype_is_cityfounder(pcity->production.value.utype)) {
      producing++;
    }
  } city_list_iterate_end;

  if (in_production != nullptr) {
    *in_production = producing;
  }

  return workers + producing;
}

/***********************************************************************//**
  Return a staged worker target for the largepox state machine.

  The opening starts with three workers and should spend its shields on city
  founders and caravans. Once most trade routes exist, add workers in waves;
  rapture needs roughly one improver per city, and mature metropolises can
  productively use about three for every two cities.
****************************************************************************/
static int dai_largepox_desired_workers(struct ai_type *ait,
                                        const struct player *pplayer)
{
  const struct ai_plr *ai = def_ai_player_data(pplayer, ait);
  int cities = city_list_size(pplayer->cities);
  enum dai_growth_state state = dai_largepox_state(ait, pplayer);

  /* Once the trade backbone exists, terrain development cannot be allowed
   * to disappear merely because a temporary food deficit puts the state
   * machine in EMERGENCY. One improver per city is the reference midgame
   * level; mature size-20 cities can use three for every two cities. */
  if (ai->largepox.route_coverage_pct >= 75) {
    return state == DAI_GROWTH_METROPOLIS
           || ai->largepox.average_core_size >= 20
           ? MAX(3, (3 * cities + 1) / 2)
           : MAX(3, cities);
  }

  /* The reference opening grows from the initial three improvers to roughly
   * ten while the route network moves from half-full to complete.  Waiting
   * for 75% coverage left Wheat/Oasis and the roads between new cities
   * untouched during the exact turns when rapture begins. */
  if (ai->largepox.route_coverage_pct >= 50) {
    return MAX(3, (3 * cities + 1) / 4);
  }

  switch (state) {
  case DAI_GROWTH_TRADE_NETWORK:
    return ai->largepox.route_coverage_pct >= 75
           ? MAX(3, (cities + 1) / 2) : MIN(3, cities);
  case DAI_GROWTH_RAPTURE_PREP:
  case DAI_GROWTH_RAPTURE:
    return MAX(3, cities);
  case DAI_GROWTH_METROPOLIS:
    return MAX(3, (3 * cities + 1) / 2);
  case DAI_GROWTH_BOOTSTRAP:
  case DAI_GROWTH_EXPAND_CORE:
    return MIN(3, cities);
  case DAI_GROWTH_EMERGENCY:
    return MIN(3, cities);
  }

  return 0;
}

/***********************************************************************//**
  The first route windfalls must capitalize the core before they create the
  midgame Worker wave.  The human reference finished Marketplace everywhere
  and had every city on Aqueduct while it still owned only the three opening
  Workers; terrain improvers grew from three to ten after that foundation was
  in place.  Without this gate, the AI crash-bought eight Workers in one turn,
  cancelling Marketplace production and delaying rapture despite a faster
  trade opening.
****************************************************************************/
static bool dai_largepox_worker_foundation_ready(struct ai_type *ait,
                                                  const struct player *pplayer)
{
  const struct impr_type *marketplace
    = improvement_by_rule_name("Marketplace");
  const struct impr_type *aqueduct = improvement_by_rule_name("Aqueduct");
  int core_cities = 0;
  int ready_cities = 0;

  if (marketplace == nullptr || aqueduct == nullptr) {
    return TRUE;
  }

  city_list_iterate(pplayer->cities, pcity) {
    if (dai_largepox_city_is_support(ait, pplayer, pcity)) {
      continue;
    }

    core_cities++;
    if (city_has_building(pcity, marketplace)
        && city_has_building(pcity, aqueduct)) {
      ready_cities++;
    }
  } city_list_iterate_end;

  return core_cities == 0 || ready_cities * 4 >= core_cities * 3;
}

/***********************************************************************//**
  Pause the first caravan wave while its arrival gold capitalizes the core.

  Half of the route endpoints already established, plus enough mobile
  caravans to cover nine tenths, is the same hand-off visible in the
  reference game.  Marketplace and Aqueduct then turn that one-time gold
  into rapture growth.  Once three quarters of the core owns both buildings,
  trade production may resume to fill the remaining route slots.
****************************************************************************/
static bool dai_largepox_capitalizing(struct ai_type *ait,
                                      const struct player *pplayer)
{
  return dai_largepox_enabled()
         && dai_largepox_actual_route_coverage(pplayer) >= 50
         && dai_largepox_projected_route_coverage(pplayer)
            >= DAI_LARGEPOX_CAPITALIZATION_PROJECTED_MIN
         && !dai_largepox_worker_foundation_ready(ait, pplayer);
}

/***********************************************************************//**
  Keep only the most advanced caravan builds needed for the capitalization
  hand-off.  Production is selected city by city, but every current build is
  visible here, so ranking by accumulated shields prevents both cancelling
  the useful final wave and preserving more caravans than the route network
  can consume.
****************************************************************************/
static bool dai_largepox_keep_trade_build(const struct player *pplayer,
                                           const struct city *pcity)
{
  int established = 0;
  int slots = 0;
  int projected;
  int target;
  int needed;
  int rank = 0;

  if (VUT_UTYPE != pcity->production.kind
      || !utype_can_do_action(pcity->production.value.utype,
                              ACTION_TRADE_ROUTE)) {
    return FALSE;
  }

  city_list_iterate(pplayer->cities, acity) {
    established += city_num_trade_routes(acity);
    slots += max_trade_routes(acity);
  } city_list_iterate_end;

  projected = established;
  unit_list_iterate(pplayer->units, punit) {
    if (unit_can_do_action(punit, ACTION_TRADE_ROUTE)) {
      projected += 2;
    }
  } unit_list_iterate_end;

  target = (DAI_LARGEPOX_CAPITALIZATION_PROJECTED_MIN * slots + 99) / 100;
  needed = MAX(0, (target - projected + 1) / 2);

  city_list_iterate(pplayer->cities, acity) {
    if (acity == pcity || VUT_UTYPE != acity->production.kind
        || !utype_can_do_action(acity->production.value.utype,
                                ACTION_TRADE_ROUTE)) {
      continue;
    }

    if (acity->shield_stock > pcity->shield_stock
        || (acity->shield_stock == pcity->shield_stock
            && acity->id < pcity->id)) {
      rank++;
    }
  } city_list_iterate_end;

  return rank < needed;
}

/***********************************************************************//**
  Maintain a bounded pipeline of dedicated workers after the trade opening.
  The ordinary one-tile value often concludes that five workers are enough
  for a twelve-city island; that ignores the strategic cost of reaching
  railroads, farmland, and terrain conversions many turns late.
****************************************************************************/
static void dai_largepox_choose_worker(struct ai_type *ait,
                                       struct player *pplayer,
                                       struct city *pcity,
                                       struct adv_choice *choice)
{
  enum dai_growth_state state = dai_largepox_state(ait, pplayer);
  struct ai_city *city_data = def_ai_city_data(pcity, ait);
  struct unit_type *worker_type;
  int in_production;
  int pipeline;
  int desired;
  int concurrent_limit;

  if (!dai_largepox_enabled() || city_data->urgency > 0
      || city_size_get(pcity) < 3) {
    return;
  }

  if (!dai_largepox_worker_foundation_ready(ait, pplayer)) {
    return;
  }

  desired = dai_largepox_desired_workers(ait, pplayer);
  pipeline = dai_largepox_worker_pipeline(pplayer, &in_production);
  concurrent_limit = state == DAI_GROWTH_METROPOLIS
                     || def_ai_player_data(pplayer, ait)
                          ->largepox.route_coverage_pct >= 50
                     ? 3 : 2;
  if (desired <= 0 || pipeline >= desired
      || in_production >= MIN(concurrent_limit, desired)) {
    return;
  }

  worker_type = dai_role_utype_for_terrain_class(pcity, UTYF_WORKERS,
                                                  TC_LAND);
  if (worker_type == nullptr || utype_is_cityfounder(worker_type)
      || !can_city_build_unit_now(&(wld.map), pcity, worker_type,
                                  RPT_CERTAIN)
      || pcity->surplus[O_SHIELD]
         <= utype_upkeep_cost(worker_type, nullptr, pplayer, O_SHIELD)) {
    return;
  }

  choice->type = CT_CIVILIAN;
  choice->value.utype = worker_type;
  choice->want = DAI_WANT_DOMESTIC_MAX;
  choice->need_boat = FALSE;
  adv_choice_set_use(choice, "largepox worker pipeline");
}

/***********************************************************************//**
  Keep a small, explicit founder pipeline while largepox is expanding.

  Founder desirability can be very high and still lose to the deliberately
  amplified caravan desire.  That produced mature seven-city islands whose
  cities all wanted a founder (often above 1500) but built only caravans and
  ordinary improvements. Guarantee a bounded two- or three-unit pipeline,
  and only in cities which
  have a real desirable site and can afford the population and food cost.
  Growth-limit buildings are applied after this and may still override the
  founder when immediately necessary.
****************************************************************************/
static void dai_largepox_choose_founder(struct ai_type *ait,
                                        struct player *pplayer,
                                        struct city *pcity,
                                        struct adv_choice *choice)
{
  struct ai_city *city_data = def_ai_city_data(pcity, ait);
  struct unit_type *founder_type;
  enum dai_growth_state state = dai_largepox_state(ait, pplayer);
  bool already_building;
  bool mature_infill;
  int missing;
  int desired_pipeline;
  int committed_founders;

  mature_infill = state == DAI_GROWTH_EXPAND_CORE
                  && city_list_size(pplayer->cities) >= 12
                  && dai_largepox_total_city_target(ait, pplayer)
                     > city_list_size(pplayer->cities);

  if (!dai_largepox_enabled()
      || (state != DAI_GROWTH_BOOTSTRAP
          && state != DAI_GROWTH_EXPAND_CORE)
      || game.scenario.prevent_new_cities
      || !player_can_do_action_result(pplayer, ACTRES_FOUND_CITY)
      || city_data->urgency > 0
      /* During the bounded 12 -> 15 wave, several cities may have cached the
       * same best site.  Requiring every producer to own a distinct local
       * founder_want serialized three Settlers into one; the completed units
       * can perform the authoritative site search with current reservations. */
      || (!mature_infill && city_data->founder_want < 50)) {
    return;
  }

  missing = dai_largepox_total_city_target(ait, pplayer)
            - city_list_size(pplayer->cities);
  committed_founders
    = city_list_size(pplayer->cities) < 12
      ? dai_largepox_mobile_founders(pplayer)
      : dai_largepox_founder_pipeline(pplayer);
  if (city_list_size(pplayer->cities) + committed_founders
      >= dai_largepox_total_city_target(ait, pplayer)) {
    return;
  }
  desired_pipeline = dai_largepox_desired_founder_pipeline(pplayer, missing);
  already_building
    = pcity->production.kind == VUT_UTYPE
      && utype_is_cityfounder(pcity->production.value.utype);
  if (already_building
      && city_list_size(pplayer->cities) >= 9
      && city_list_size(pplayer->cities) < 12
      && !dai_largepox_founder_build_committed(
           pplayer, pcity,
           dai_largepox_total_city_target(ait, pplayer)
           - city_list_size(pplayer->cities)
           - dai_largepox_mobile_founders(pplayer))) {
    return;
  }
  if (desired_pipeline <= 0
      || (!already_building
          && dai_largepox_founder_pipeline(pplayer)
             >= desired_pipeline)) {
    return;
  }

  founder_type = best_role_unit(pcity,
                                action_id_get_role(ACTION_FOUND_CITY));
  /* It is safe to begin accumulating shields at size one: the server city
   * manager remains food-first until population exceeds the founder's
   * pop_cost. In Republic/Democracy the finished Settler can cost two food,
   * while the rapture governor deliberately keeps only +1; requiring all of
   * that future upkeep made late infill impossible even in size-20 cities.
   * Positive food is enough to begin production and the manager rearranges
   * when the unit actually appears. */
  if (founder_type == nullptr || pcity->surplus[O_FOOD] <= 0) {
    return;
  }

  choice->type = CT_CIVILIAN;
  choice->value.utype = founder_type;
  /* Beat routine garrison/building preferences, but only after the urgency
   * guard above has established that there is no military emergency. */
  choice->want = DAI_WANT_MILITARY_EMERGENCY * 1000;
  choice->need_boat = city_data->founder_want >= 50
                      && city_data->founder_boat;
  adv_choice_set_use(choice, "largepox founder pipeline");
}

/***********************************************************************//**
  Preserve the opening Settler builds as unit-class shield banks for Trade.

  The human reference kept every city on Settlers until Trade arrived and
  then changed the complete core to Caravans. The generic AI considered the
  city target complete several turns earlier and changed half the empire to
  Barracks or Coinage. Hard AI obeys the production-change penalty, so that
  apparently harmless detour discarded half of the shields and delayed the
  route windfall by four to six turns.

  Extra Settlers which finish during this short window are safe: the bounded
  city-target logic cancels surplus city missions and joins them into the
  smallest core city. Thus their population is retained instead of creating
  an oversized opening core.
****************************************************************************/
static void dai_largepox_choose_trade_prebuild(struct ai_type *ait,
                                                struct player *pplayer,
                                                struct city *pcity,
                                                struct adv_choice *choice)
{
  const struct advance *trade = advance_by_rule_name("Trade");
  struct unit_type *founder_type;

  if (!dai_largepox_enabled()
      || trade == nullptr
      || research_invention_state(research_get(pplayer),
                                  advance_number(trade)) == TECH_KNOWN
      || city_list_size(pplayer->cities) < 9
      || def_ai_city_data(pcity, ait)->urgency > 0) {
    return;
  }

  founder_type = best_role_unit(pcity,
                                action_id_get_role(ACTION_FOUND_CITY));
  if (founder_type == nullptr) {
    return;
  }

  choice->type = CT_CIVILIAN;
  choice->value.utype = founder_type;
  choice->want = DAI_WANT_MILITARY_EMERGENCY * 1500;
  choice->need_boat = FALSE;
  adv_choice_set_use(choice, "largepox trade prebuild");
}

/***********************************************************************//**
  Convert surplus speculative Settler builds directly into caravans.

  Starting at nine cities, retain only the founder builds needed for the last
  opening sites.  Request the exact empire-wide caravan wave still needed to
  fill the route endpoints; surplus founder builds keep their accumulated
  unit shields and join that wave.  This is a production handoff, not a
  permanent caravan override; the normal trade and economic advisors take
  over after the opening.
****************************************************************************/
static void dai_largepox_choose_trade_handoff(struct ai_type *ait,
                                               struct player *pplayer,
                                               struct city *pcity,
                                               struct adv_choice *choice)
{
  struct ai_city *city_data = def_ai_city_data(pcity, ait);
  struct unit_type *trade_type;
  const struct ai_plr *ai = def_ai_player_data(pplayer, ait);
  int cities = city_list_size(pplayer->cities);
  int target = dai_largepox_total_city_target(ait, pplayer);
  int slots = target - cities - dai_largepox_mobile_founders(pplayer);
  int missing_route_endpoints
    = MAX(0, ai->largepox.route_slots - ai->largepox.established_routes);
  int desired_caravans = (missing_route_endpoints + 1) / 2;
  bool building_founder
    = pcity->production.kind == VUT_UTYPE
      && utype_is_cityfounder(pcity->production.value.utype);

  if (!dai_largepox_enabled()
      || dai_largepox_city_is_support(ait, pplayer, pcity)
      || city_data->urgency > 0
      || cities < 9
      || dai_largepox_capitalizing(ait, pplayer)
      || desired_caravans <= 0
      || dai_largepox_trade_unit_pipeline(pplayer) >= desired_caravans
      || dai_largepox_routes_in_pipeline(pcity) >= max_trade_routes(pcity)
      || (building_founder
          && dai_largepox_founder_build_committed(pplayer, pcity, slots))) {
    return;
  }

  trade_type = best_role_unit(pcity,
                              action_id_get_role(ACTION_TRADE_ROUTE));
  if (trade_type == nullptr
      || !can_city_build_unit_now(&(wld.map), pcity, trade_type,
                                  RPT_CERTAIN)) {
    return;
  }

  choice->type = CT_CIVILIAN;
  choice->value.utype = trade_type;
  /* This must beat the previous turn's speculative founder choice, which is
   * intentionally very sticky and otherwise survives adv_better_choice(). */
  choice->want = DAI_WANT_MILITARY_EMERGENCY * 2000;
  choice->need_boat = FALSE;
  adv_choice_set_use(choice, "largepox founder-to-trade handoff");
}

/***********************************************************************//**
  A rapturing city at its current population ceiling must remove that block
  before any caravan, worker, or ordinary economic building can help it.
  Select by effects rather than by ruleset names so Aqueduct/Sewer
  equivalents work too.
****************************************************************************/
static void dai_largepox_choose_growth_building(struct ai_type *ait,
                                                 struct city *pcity,
                                                 struct adv_choice *choice)
{
  const struct impr_type *chosen = nullptr;
  adv_want chosen_want = 0;
  int size_limit = get_city_bonus(pcity, EFT_SIZE_ADJ);
  enum dai_growth_state state
    = dai_largepox_state(ait, city_owner(pcity));

  if (!dai_largepox_enabled()
      || get_city_bonus(pcity, EFT_SIZE_UNLIMIT) > 0
      || (city_can_grow_to(pcity, city_size_get(pcity) + 1)
          && (state == DAI_GROWTH_TRADE_NETWORK
              || city_size_get(pcity) < MAX(1, size_limit - 1)))) {
    return;
  }

  improvement_iterate(pimprove) {
    adv_want want;

    if ((!building_has_effect(pimprove, EFT_SIZE_ADJ)
         && !building_has_effect(pimprove, EFT_SIZE_UNLIMIT))
        || !can_city_build_improvement_now(pcity, pimprove, RPT_CERTAIN)) {
      continue;
    }

    want = pcity->server.adv->building_want[improvement_index(pimprove)];
    if (chosen == nullptr || want > chosen_want) {
      chosen = pimprove;
      chosen_want = want;
    }
  } improvement_iterate_end;

  if (chosen != nullptr) {
    choice->type = CT_BUILDING;
    choice->value.building = chosen;
    choice->want = DAI_WANT_DOMESTIC_MAX;
    choice->need_boat = FALSE;
    adv_choice_set_use(choice, "largepox growth limit");
  }
}

/***********************************************************************//**
  Build the economic backbone in the same stable order as a human largepox
  opening.  Recurring route yields are not represented in the ordinary
  worked-tile building estimate, so that advisor can otherwise prefer walls
  or units indefinitely even in safe cities with three or four routes.

  Skip temporarily unavailable entries instead of blocking the whole chain:
  for example Harbour can be built while Banking is still being researched,
  and the missing Bank becomes first priority as soon as it is legal.
****************************************************************************/
static void dai_largepox_choose_economic_chain(struct ai_type *ait,
                                                struct city *pcity,
                                                struct adv_choice *choice)
{
  static const char *const chain[] = {
    "Marketplace",
    "Aqueduct",
    "Bank",
    "Harbour",
    "Stock Exchange",
    "Library",
    "Sewer System",
    "University",
    "Factory",
    "Offshore Platform",
    "Supermarket",
    "Mass Transit"
  };
  enum dai_growth_state state
    = dai_largepox_state(ait, city_owner(pcity));
  int actual_coverage
    = dai_largepox_actual_route_coverage(city_owner(pcity));
  int projected_coverage
    = dai_largepox_projected_route_coverage(city_owner(pcity));
  bool support
    = dai_largepox_city_is_support(ait, city_owner(pcity), pcity);
  int urgency = def_ai_city_data(pcity, ait)->urgency;
  size_t empire_frontier = ARRAY_SIZE(chain);

  CITY_LOG(LOG_DEBUG, pcity,
           "largepox economic chain: state=%s actual=%d projected=%d "
           "support=%d urgency=%d",
           dai_largepox_state_name(state), actual_coverage,
           projected_coverage, support, urgency);

  if (!dai_largepox_enabled()
      || (state != DAI_GROWTH_TRADE_NETWORK
          && state != DAI_GROWTH_RAPTURE_PREP
          && state != DAI_GROWTH_RAPTURE
          && state != DAI_GROWTH_METROPOLIS
          && !(state == DAI_GROWTH_EMERGENCY
               && actual_coverage >= 50)
          && !(state == DAI_GROWTH_EXPAND_CORE
               && city_list_size(city_owner(pcity)->cities) >= 12)
          /* Strategic state is reconstructed after loading a save and can
           * read bootstrap for the first resumed AI phase.  The map itself
           * is authoritative: a twelve-city empire with half its route
           * endpoints established is already in capitalization regardless
           * of that transient label. */
          && !(city_list_size(city_owner(pcity)->cities) >= 12
               && actual_coverage >= 50))
      || support
      /* Urgency one is the ordinary desire for a token defender, not a real
       * emergency.  Let the core keep capitalizing; stronger danger still
       * pre-empts the chain. */
      || urgency > 1
      || (actual_coverage < 50
          && projected_coverage < 50)) {
    return;
  }

  if (projected_coverage < DAI_LARGEPOX_CAPITALIZATION_PROJECTED_MIN
      && dai_largepox_keep_trade_build(city_owner(pcity), pcity)) {
    /* Do not cancel a nearly-complete caravan merely because another city
     * established the 50% endpoint which opened Marketplace.  The human
     * reference keeps this last wave moving while the rest of the core
     * capitalizes in parallel. */
    choice->type = CT_CIVILIAN;
    choice->value.utype = pcity->production.value.utype;
    choice->want = DAI_WANT_MILITARY_EMERGENCY * 600;
    choice->need_boat = FALSE;
    adv_choice_set_use(choice, "largepox final trade wave");
    return;
  }

  /* Keep the dense core on a common economic frontier.  A purely local
   * chain lets the best production cities reach Stock Exchange, Library,
   * and Sewer System while peripheral cities still lack Marketplace or
   * Aqueduct.  The reference largepox opening instead spreads the route
   * windfall over the whole core.  A city may pipeline one tier beyond the
   * least-developed core city, but never run several tiers ahead. */
  city_list_iterate(city_owner(pcity)->cities, acity) {
    if (dai_largepox_city_is_support(ait, city_owner(pcity), acity)) {
      continue;
    }

    for (size_t i = 0; i < ARRAY_SIZE(chain); i++) {
      const struct impr_type *building = improvement_by_rule_name(chain[i]);

      if (building != nullptr && !city_has_building(acity, building)) {
        empire_frontier = MIN(empire_frontier, i);
        break;
      }
    }
  } city_list_iterate_end;

  for (size_t i = 0; i < ARRAY_SIZE(chain); i++) {
    struct impr_type *building = improvement_by_rule_name(chain[i]);

    if ((empire_frontier < ARRAY_SIZE(chain) && i > empire_frontier + 1)
        || building == nullptr || city_has_building(pcity, building)
        || !can_city_build_improvement_now(pcity, building, RPT_CERTAIN)
        /* Start Marketplace as soon as half of the route endpoints are
         * either established or carried by caravans already on the map.
         * Route windfalls arrive before the whole network is complete, and
         * leaving that gold idle costs several rapture growth turns.  Keep
         * Bank and the rest of the chain behind a stronger 75% commitment
         * so unfinished caravan production is not cancelled too early. */
        || (i > 0 && projected_coverage < 75)) {
      continue;
    }

    choice->type = CT_BUILDING;
    choice->value.building = building;
    /* Beat stale peacetime military choices retained from the previous
     * turn.  Urgent cities were excluded above, so this cannot suppress a
     * real defense emergency. */
    choice->want = DAI_WANT_MILITARY_EMERGENCY * 500;
    choice->need_boat = FALSE;
    adv_choice_set_use(choice, "largepox economic chain");
    CITY_LOG(LOG_DEBUG, pcity,
             "largepox economic chain selects %s",
             improvement_rule_name(building));
    return;
  }
}

/***********************************************************************//**
  Expand the workable city map before rapture.  On archipelagos this normally
  selects Harbour, whose outer ocean tiles also receive the food needed to
  sustain a large city.  Select by effect so equivalent ruleset buildings
  work without hard-coding their names.
****************************************************************************/
static void dai_largepox_choose_radius_building(struct ai_type *ait,
                                                struct city *pcity,
                                                struct adv_choice *choice)
{
  const struct impr_type *chosen = nullptr;
  int chosen_cost = FC_INFINITY;
  enum dai_growth_state state
    = dai_largepox_state(ait, city_owner(pcity));

  if (!dai_largepox_enabled()
      || dai_largepox_city_is_support(ait, city_owner(pcity), pcity)
      || (state != DAI_GROWTH_TRADE_NETWORK
          && state != DAI_GROWTH_RAPTURE_PREP
          && state != DAI_GROWTH_RAPTURE
          && state != DAI_GROWTH_METROPOLIS)
      || get_city_bonus(pcity, EFT_CITY_RADIUS_SQ) > 0) {
    return;
  }

  if (state == DAI_GROWTH_TRADE_NETWORK
      && dai_largepox_routes_in_pipeline(pcity) < max_trade_routes(pcity)) {
    /* Do not let Harbour pre-empt the caravans which define this phase. Once
     * every local slot is established or already en route, vertical growth
     * may safely consume this city's production. */
    return;
  }

  improvement_iterate(pimprove) {
    int cost;

    if (!building_has_effect(pimprove, EFT_CITY_RADIUS_SQ)
        || !can_city_build_improvement_now(pcity, pimprove, RPT_CERTAIN)) {
      continue;
    }

    cost = impr_build_shield_cost(pcity, pimprove);
    if (chosen == nullptr || cost < chosen_cost) {
      chosen = pimprove;
      chosen_cost = cost;
    }
  } improvement_iterate_end;

  if (chosen != nullptr) {
    choice->type = CT_BUILDING;
    choice->value.building = chosen;
    choice->want = DAI_WANT_DOMESTIC_MAX;
    choice->need_boat = FALSE;
    adv_choice_set_use(choice, "largepox city radius");
  }
}


/***********************************************************************//**
  Evaluate the need for units (like caravans) that aid wonder construction.
  If another city is building wonder and needs help but pplayer is not
  advanced enough to build caravans, the corresponding tech will be
  stimulated.
****************************************************************************/
static void dai_choose_help_wonder(struct ai_type *ait,
                                   struct city *pcity,
                                   struct adv_choice *choice,
                                   struct adv_data *ai)
{
  struct player *pplayer = city_owner(pcity);
  Continent_id continent = tile_continent(pcity->tile);
  struct ai_city *city_data = def_ai_city_data(pcity, ait);
  /* Total count of caravans available or already being built
   * on this continent */
  int caravans = 0;
  /* The type of the caravan */
  struct unit_type *unit_type;
  struct city *wonder_city = game_city_by_number(ai->wonder_city);

  if (num_role_units(action_id_get_role(ACTION_HELP_WONDER)) == 0) {
    /* No such units available in the ruleset */
    return;
  }

  if (pcity == wonder_city
      || wonder_city == NULL
      || city_data->distance_to_wonder_city <= 0
      || !city_production_gets_caravan_shields(&wonder_city->production)
      /* TODO: Should helping to build a unit be considered when legal? */
      || VUT_UTYPE == wonder_city->production.kind
      /* TODO: Should helping to build an improvement be considered when
       * legal? */
      || !is_wonder(wonder_city->production.value.building)) {
    /* A distance of zero indicates we are very far away, possibly
     * on another continent. */
    return;
  }

  /* Count existing caravans */
  unit_list_iterate(pplayer->units, punit) {
    if (unit_can_do_action(punit, ACTION_HELP_WONDER)
        && tile_continent(unit_tile(punit)) == continent)
      caravans++;
  } unit_list_iterate_end;

  /* Count caravans being built */
  city_list_iterate(pplayer->cities, acity) {
    if (VUT_UTYPE == acity->production.kind
        && utype_can_do_action(acity->production.value.utype,
                               ACTION_HELP_WONDER)
        && tile_continent(acity->tile) == continent) {
      caravans++;
    }
  } city_list_iterate_end;

  unit_type = best_role_unit(pcity, action_id_get_role(ACTION_HELP_WONDER));

  if (!unit_type) {
    /* We cannot build such units yet
     * but we will consider it to stimulate science */
    unit_type = get_role_unit(action_id_get_role(ACTION_HELP_WONDER), 0);
  }

  fc_assert_msg(utype_can_do_action(unit_type, ACTION_HELP_WONDER),
                "Non existence of wonder helper unit not caught");

  /* Check if wonder needs a little help. */
  if (build_points_left(wonder_city)
      > utype_build_shield_cost_base(unit_type) * caravans) {
    const struct impr_type *wonder = wonder_city->production.value.building;
    adv_want want = wonder_city->server.adv->building_want[improvement_index(wonder)];
    int dist = city_data->distance_to_wonder_city /
               unit_type->move_rate;

    fc_assert_ret(VUT_IMPROVEMENT == wonder_city->production.kind);

    want /= MAX(dist, 1);
    CITY_LOG(LOG_DEBUG, pcity, "want %s to help wonder in %s with " ADV_WANT_PRINTF,
             utype_rule_name(unit_type),
             city_name_get(wonder_city),
             want);
    if (want > choice->want) {
      /* This sets our tech want in cases where we cannot actually build
       * the unit. */
      unit_type = dai_wants_role_unit(ait, pplayer, pcity,
                                      action_id_get_role(ACTION_HELP_WONDER),
                                      want);
      if (unit_type != NULL) {
        choice->want = want;
        choice->type = CT_CIVILIAN;
        choice->value.utype = unit_type;
      } else {
        CITY_LOG(LOG_DEBUG, pcity,
                 "would but could not build ACTION_HELP_WONDER unit, "
                 "bumped reqs");
      }
    }
  }
}

/***********************************************************************//**
  Evaluate the need for units (like caravans) that create trade routes.
  If pplayer is not advanced enough to build caravans, the corresponding
  tech will be stimulated.
***************************************************************************/
static void dai_choose_trade_route(struct ai_type *ait, struct city *pcity,
                                   struct adv_choice *choice,
                                   struct adv_data *ai)
{
  struct player *pplayer = city_owner(pcity);
  struct unit_type *unit_type;
  adv_want want;
  int income, bonus;
  int trade_routes;
  int caravan_units;
  int unassigned_caravans;
  int max_routes;
  Continent_id continent = tile_continent(pcity->tile);
  bool dest_city_found = FALSE;
  bool dest_city_nat_different_cont = FALSE;
  bool dest_city_nat_same_cont = FALSE;
  bool dest_city_in_different_cont = FALSE;
  bool dest_city_in_same_cont = FALSE;
  bool prefer_different_cont;
  int pct = 0;
  int trader_trait;
  bool can_move_ic = FALSE, has_boats = TRUE;
  bool need_boat = FALSE;
  int trade_action;

  if (dai_largepox_enabled()
      && (dai_largepox_city_is_support(ait, pplayer, pcity)
          || dai_largepox_capitalizing(ait, pplayer))) {
    return;
  }

  if (city_list_size(pplayer->cities) < 5) {
    /* Consider trade routes only if enough destination cities.
     * This is just a quick check. We make more detailed check below. */
    return;
  }

  if (num_role_units(action_id_get_role(ACTION_TRADE_ROUTE)) == 0
      && num_role_units(action_id_get_role(ACTION_MARKETPLACE)) == 0) {
    /* No such units available in the ruleset */
    return;
  }

  unit_type = best_role_unit(pcity,
                             action_id_get_role(ACTION_TRADE_ROUTE));

  if (!unit_type) {
    /* Can't establish trade route yet. What about entering a marketplace? */
    /* TODO: Should a future unit capable of establishing trade routes be
     * prioritized above a present unit capable of entering a market place?
     * In that case this should be below the check for a future unit
     * capable of establishing a trade route. */
    unit_type = best_role_unit(pcity,
                               action_id_get_role(ACTION_MARKETPLACE));
  }

  if (!unit_type) {
    /* We cannot build such units yet
     * but we will consider it to stimulate science */
    unit_type = get_role_unit(action_id_get_role(ACTION_TRADE_ROUTE), 0);
  }

  if (!unit_type) {
    /* We'll never be able to establish a trade route. Consider a unit that
     * can enter the marketplace instead, to stimulate science. */
    unit_type = get_role_unit(action_id_get_role(ACTION_MARKETPLACE), 0);
  }

  fc_assert_msg(unit_type,
                "Non-existence of trade unit not caught");

  if (unit_type) {
    struct unit_class *pclass = utype_class(unit_type);

    /* Hope there is no "water cities in lakes" ruleset? */
    can_move_ic = pclass->adv.sea_move != MOVE_NONE;
    has_boats = pclass->adv.ferry_types > 0;
  }

  if (trade_route_type_trade_pct(TRT_NATIONAL_IC) >
      trade_route_type_trade_pct(TRT_NATIONAL)
      && (can_move_ic || has_boats)) {
    prefer_different_cont = TRUE;
  } else {
    prefer_different_cont = FALSE;
  }

  /* The largepox opening is an internal island network.  Chasing the larger
   * intercontinental percentage here made every city request a Trireme even
   * though eleven terrestrial partners were available.  Overseas routes can
   * return to the ordinary preference after this specialized opening. */
  if (dai_largepox_enabled()) {
    prefer_different_cont = FALSE;
  }

  /* Look for proper destination city for trade. */
  if (trade_route_type_trade_pct(TRT_NATIONAL) > 0
      || trade_route_type_trade_pct(TRT_NATIONAL_IC) > 0) {
    /* National trade routes have value */
    city_list_iterate(pplayer->cities, acity) {
      if (can_cities_trade(pcity, acity)) {
        if (tile_continent(acity->tile) != continent) {
          if (!(can_move_ic || has_boats)) {
            /* FIXME: get by roads/rivers? */
            continue;
          }
          dest_city_found = TRUE;
          dest_city_nat_different_cont = TRUE;
          if (prefer_different_cont) {
            break;
          }
        } else {
          dest_city_found = TRUE;
          dest_city_nat_same_cont = TRUE;
          if (!prefer_different_cont) {
            break;
          }
        }
      }
    } city_list_iterate_end;
  }

  /* FIXME: This check should consider more about relative
   * income from different trade route types. This works just
   * with more typical ruleset setups. */
  if (prefer_different_cont && !dest_city_nat_different_cont) {
    if (trade_route_type_trade_pct(TRT_IN_IC) >
        trade_route_type_trade_pct(TRT_IN)) {
      prefer_different_cont = TRUE;
    } else {
      prefer_different_cont = FALSE;
    }

    players_iterate(aplayer) {
      if (aplayer == pplayer || !aplayer->is_alive) {
	continue;
      }
      if (pplayers_allied(pplayer, aplayer)) {
        city_list_iterate(aplayer->cities, acity) {
          if (can_cities_trade(pcity, acity)) {
            if (tile_continent(acity->tile) != continent) {
              if (!(can_move_ic || has_boats)) {
                /* FIXME: get by roads/rivers? */
                continue;
              }
              dest_city_found = TRUE;
              dest_city_in_different_cont = TRUE;
              if (prefer_different_cont) {
                break;
              }
            } else {
              dest_city_found = TRUE;
              dest_city_in_same_cont = TRUE;
              if (!prefer_different_cont) {
                break;
              }
            }
          }
        } city_list_iterate_end;
      }
      if ((dest_city_in_different_cont && prefer_different_cont)
          || (dest_city_in_same_cont && !prefer_different_cont)) {
        break;
      }
    } players_iterate_end;
  }

  if (!dest_city_found) {
    /* No proper destination city. */
    return;
  }

  trade_routes = city_num_trade_routes(pcity);
  /* Count also caravans enroute to establish trade routes */
  caravan_units = 0;
  unit_list_iterate(pcity->units_supported, punit) {
    if (unit_can_do_action(punit, ACTION_TRADE_ROUTE)) {
      caravan_units++;
    }
  } unit_list_iterate_end;

  max_routes = max_trade_routes(pcity);
  unassigned_caravans = caravan_units - (max_routes - trade_routes);
  trade_routes += caravan_units;

  /* We consider only initial benefit from establishing trade route.
   * We may actually get only initial benefit if both cities already
   * have four trade routes, if there already is route between them
   * or if the Establish Trade Route action is illegal. */

  /* The calculations of get_caravan_enter_city_trade_bonus() have to be
   * duplicated here because the city traded with is imaginary. */

  /* We assume that we are creating trade route to city with 75% of
   * pcitys trade 10 squares away (FIXME: another estimation?). */
  if (CBS_LOGARITHMIC == game.info.caravan_bonus_style) {
    int wd = ((100 - game.info.trade_world_rel_pct) * 10
               + game.info.trade_world_rel_pct
                 * (10 * 40 / MAX(MAP_NATIVE_WIDTH, MAP_NATIVE_HEIGHT))) / 100;

    income = pow(log(wd + 20 + 1.75 * max_trade_prod(pcity)) * 2, 2);
  } else /* assume CBS_CLASSIC */ {
    income = (10 + 10) * (1.75 * pcity->surplus[O_TRADE]) / 24;
  }

  /* A ruleset may use the Trade_Revenue_Bonus effect to reduce the one
   * time bonus if no trade route is established. Make sure it gets the
   * correct action. */
  trade_action = utype_can_do_action(unit_type, ACTION_TRADE_ROUTE) ?
        ACTION_TRADE_ROUTE : ACTION_MARKETPLACE;
  bonus = get_target_bonus_effects(NULL,
                                   &(const struct req_context) {
                                     .player = pplayer,
                                     .city = pcity,
                                     .tile = city_tile(pcity),
                                     .unittype = unit_type,
                                     .action =
                                       action_by_number(trade_action),
                                   },
                                   NULL,
                                   EFT_TRADE_REVENUE_BONUS);

  /* Be mercy full to players with small amounts. Round up. */
  income = ceil((float)income * pow(2.0, (double)bonus / 1000.0));

  if (dest_city_nat_same_cont) {
    pct = trade_route_type_trade_pct(TRT_NATIONAL);
  }
  if (dest_city_in_same_cont) {
    int typepct = trade_route_type_trade_pct(TRT_IN);

    pct = MAX(pct, typepct);
  }
  if (dest_city_nat_different_cont) {
    int typepct = trade_route_type_trade_pct(TRT_NATIONAL_IC);

    if (typepct > pct) {
      pct = typepct;
      need_boat = TRUE;
    }
  }
  if (dest_city_in_different_cont) {
    int typepct = trade_route_type_trade_pct(TRT_IN_IC);

    if (typepct > pct) {
      pct = typepct;
      need_boat = TRUE;
    }
  }

  need_boat = need_boat && !can_move_ic;
  if (dai_largepox_enabled() && dest_city_nat_same_cont) {
    need_boat = FALSE;
  }
  income = pct * income / 100;

  want = income * ai->gold_priority + income * ai->science_priority;

  /* We get this income only once after all.
   * This value is adjusted for most interesting gameplay.
   * For optimal performance AI should build more caravans, but
   * we want it to build less valued buildings too. */
  trader_trait = ai_trait_get_value(TRAIT_TRADER, pplayer);
  want /= (130 * TRAIT_DEFAULT_VALUE / trader_trait);

  /* Increase want for trade routes if our economy is very weak.
   * We may have enough gold, but only because we have already set
   * tax rate to 100%. So don't check gold directly, but tax rate.
   * This method helps us out of deadlocks of completely stalled
   * scientific progress.
   */
  if (pplayer->economic.science < 50 && trade_routes < max_routes
      && utype_can_do_action(unit_type, ACTION_TRADE_ROUTE)) {
    want *=
      (6 - pplayer->economic.science / 10) * (6 - pplayer->economic.science / 10);
  }

  if (trade_routes == 0 && max_routes > 0
      && utype_can_do_action(unit_type, ACTION_TRADE_ROUTE)) {
    /* If we have no trade routes at all, we are certainly creating a new one. */
    want += trader_trait;
  } else if (trade_routes < max_routes
             && utype_can_do_action(unit_type, ACTION_TRADE_ROUTE)) {
    /* Possibly creating a new trade route */
    want += trader_trait / 4;
  }

  if (dai_largepox_enabled() && trade_routes < max_routes
      && utype_can_do_action(unit_type, ACTION_TRADE_ROUTE)) {
    switch (dai_largepox_state(ait, pplayer)) {
    case DAI_GROWTH_EXPAND_CORE:
      want = want * 3 / 2;
      break;
    case DAI_GROWTH_TRADE_NETWORK:
      want = MAX(want * 10,
                 (max_routes - trade_routes) * trader_trait * 3);
      break;
    case DAI_GROWTH_RAPTURE_PREP:
      want = MAX(want * 8,
                 (max_routes - trade_routes) * trader_trait * 2);
      break;
    case DAI_GROWTH_RAPTURE:
      /* Grow vertically without abandoning the unfinished trade network. */
      want = MAX(want * 6,
                 (max_routes - trade_routes) * trader_trait * 2);
      break;
    case DAI_GROWTH_METROPOLIS:
      want = MAX(want * 4,
                 (max_routes - trade_routes) * trader_trait);
      break;
    case DAI_GROWTH_BOOTSTRAP:
    case DAI_GROWTH_EMERGENCY:
      break;
    }
  }

  want -= utype_build_shield_cost(pcity, NULL, unit_type) * SHIELD_WEIGHTING / 150;

  /* Don't pile too many of them */
  if (unassigned_caravans * 10 > want && want > 0.0) {
    want = 0.1;
  } else {
    want -= unassigned_caravans * 10; /* Don't pile too many of them */
  }

  CITY_LOG(LOG_DEBUG, pcity,
           "want for trade route unit is " ADV_WANT_PRINTF " (expected initial income %d)",
           want,
           income);

  if (want > choice->want) {
    /* This sets our tech want in cases where we cannot actually build
     * the unit. */
    unit_type = dai_wants_role_unit(ait, pplayer, pcity,
                                    action_id_get_role(ACTION_TRADE_ROUTE),
                                    want);

    if (unit_type == NULL) {
      unit_type = dai_wants_role_unit(ait, pplayer, pcity,
                                      action_id_get_role(ACTION_MARKETPLACE),
                                      want);
    }

    /* We don't want to build caravan that wouldn't be able to reach the
     * destination by any means. It would be useless. */
    if (unit_type != NULL && (!need_boat || has_boats)) {
      choice->want = want;
      choice->type = CT_CIVILIAN;
      choice->value.utype = unit_type;
      choice->need_boat = need_boat;
    } else {
      CITY_LOG(LOG_DEBUG, pcity,
               "would but could not build trade route unit, bumped reqs");
    }
  }
}

/***********************************************************************//**
  This function should fill the supplied choice structure.

  If want is 0, this advisor doesn't want anything.
***************************************************************************/
struct adv_choice *domestic_advisor_choose_build(struct ai_type *ait,
                                                 struct player *pplayer,
                                                 struct city *pcity)
{
  struct adv_data *adv = adv_data_get(pplayer, NULL);
  /* Unit type with certain role */
  struct unit_type *worker_type;
  struct unit_type *founder_type;
  adv_want worker_want, founder_want;
  struct ai_city *city_data = def_ai_city_data(pcity, ait);
  struct adv_choice *choice = adv_new_choice();

  /* Find out desire for workers (terrain improvers) */
  worker_type = city_data->worker_type;

  /* The worker want is calculated in daicity.c called from
   * dai_manage_cities(). The expand value is the % that the AI should
   * value expansion (basically to handicap easier difficulty levels)
   * and is set when the difficulty level is changed (difficulty.c). */
  worker_want = city_data->worker_want * pplayer->ai_common.expand / 100;

  if (dai_largepox_enabled()) {
    if (!dai_largepox_worker_foundation_ready(ait, pplayer)
        && dai_largepox_worker_pipeline(pplayer, nullptr)
           >= MIN(3, city_list_size(pplayer->cities))) {
      /* Keep the three opening improvers, but do not let the generic worker
       * advisor bypass the Marketplace/Aqueduct capitalization gate. */
      worker_want = 0;
    } else if (dai_largepox_city_is_support(ait, pplayer, pcity)) {
      worker_want *= 2;
    } else if (dai_largepox_growth_focus(ait, pplayer)) {
      worker_want = worker_want * 3 / 2;
    }
  }

  if (adv->wonder_city == pcity->id) {
    if (worker_type == NULL || worker_type->pop_cost > 0) {
      worker_want /= 5;
    } else {
      worker_want /= 2;
    }
  }

  if (worker_type != NULL
      && pcity->surplus[O_FOOD] > utype_upkeep_cost(worker_type, nullptr,
                                                    pplayer, O_FOOD)) {
    if (worker_want > 0) {
      CITY_LOG(LOG_DEBUG, pcity,
               "desires terrain improvers with passion " ADV_WANT_PRINTF,
               worker_want);
      dai_choose_role_unit(ait, pplayer, pcity, choice, CT_CIVILIAN,
                           UTYF_WORKERS, worker_want, FALSE);
      adv_choice_set_use(choice, "worker");
    }
    /* Terrain improvers don't use boats (yet) */

  } else if (worker_type == NULL && worker_want > 0) {
    /* Can't build workers. Lets stimulate science */
    dai_wants_role_unit(ait, pplayer, pcity, UTYF_WORKERS, worker_want);
  }

  /* Find out desire for city founders */
  /* Basically, copied from above and adjusted. -- jjm */
  if (!game.scenario.prevent_new_cities) {
    founder_type = best_role_unit(pcity,
                                  action_id_get_role(ACTION_FOUND_CITY));

    /* founder_want calculated in daisettlers.c */
    founder_want = city_data->founder_want;

    if (dai_largepox_enabled()) {
      bool support = dai_largepox_city_is_support(ait, pplayer, pcity);
      bool building_founder
        = pcity->production.kind == VUT_UTYPE
          && utype_is_cityfounder(pcity->production.value.utype);
      int founder_pipeline = dai_largepox_founder_pipeline(pplayer);
      int committed_founders
        = city_list_size(pplayer->cities) < 12
          ? dai_largepox_mobile_founders(pplayer)
          : founder_pipeline;
      int missing = dai_largepox_total_city_target(ait, pplayer)
                    - city_list_size(pplayer->cities);

      if (city_list_size(pplayer->cities)
          + committed_founders
          >= dai_largepox_total_city_target(ait, pplayer)) {
        founder_want = 0;
      } else {
        switch (dai_largepox_state(ait, pplayer)) {
        case DAI_GROWTH_BOOTSTRAP:
          if (!building_founder
              && founder_pipeline
                 >= dai_largepox_desired_founder_pipeline(pplayer, missing)) {
            founder_want = 0;
            break;
          }
          founder_want *= 3;
          break;
        case DAI_GROWTH_EXPAND_CORE:
          if (!building_founder
              && founder_pipeline
                 >= dai_largepox_desired_founder_pipeline(pplayer, missing)) {
            founder_want = 0;
            break;
          }
          if (support) {
            founder_want *= 4;
          } else {
            founder_want *= 3;
          }
          break;
        case DAI_GROWTH_TRADE_NETWORK:
        case DAI_GROWTH_RAPTURE_PREP:
        case DAI_GROWTH_RAPTURE:
        case DAI_GROWTH_METROPOLIS:
          founder_want /= 100;
          break;
        case DAI_GROWTH_EMERGENCY:
          founder_want = 0;
          break;
        }
      }

      /* At most two transports are needed for the bounded 12 -> 15 infill.
       * Production changes are applied city by city, so this count also
       * prevents every remaining city from selecting a ferry in the same
       * advisor pass. */
      if (founder_want < 0
          && dai_largepox_ferry_pipeline(pplayer)
             >= MIN(2, MAX(1, missing))) {
        founder_want = 0;
      }
    }

    if (adv->wonder_city == pcity->id) {
      if (founder_type == NULL || founder_type->pop_cost > 0) {
        founder_want /= 5;
      } else {
        founder_want /= 2;
      }
    }

    if (adv->max_num_cities <= city_list_size(pplayer->cities)
        || !player_can_do_action_result(pplayer, ACTRES_FOUND_CITY)) {
      founder_want /= 100;
    }

    /* Adjust founder want by traits */
    founder_want *= (double)ai_trait_get_value(TRAIT_EXPANSIONIST, pplayer)
      / TRAIT_DEFAULT_VALUE;

    if (founder_type
        && pcity->surplus[O_FOOD] >= utype_upkeep_cost(founder_type, nullptr,
                                                       pplayer, O_FOOD)) {

      if (founder_want > choice->want) {
        CITY_LOG(LOG_DEBUG, pcity,
                 "desires founders with passion " ADV_WANT_PRINTF,
                 founder_want);
        dai_choose_role_unit(ait, pplayer, pcity, choice, CT_CIVILIAN,
                             action_id_get_role(ACTION_FOUND_CITY),
                             founder_want,
                             city_data->founder_boat);
        adv_choice_set_use(choice, "founder");

      } else if (founder_want < -choice->want) {
        /* We need boats to colonize! */
        /* We might need boats even if there are boats free,
         * if they are blockaded or in inland seas. */
        struct ai_plr *ai = dai_plr_data_get(ait, pplayer, NULL);

        CITY_LOG(LOG_DEBUG, pcity,
                 "desires founders with passion " ADV_WANT_PRINTF
                 " and asks for a new boat (%d of %d free)",
                 -founder_want, ai->stats.available_boats, ai->stats.boats);

        /* First fill choice with founder information */
        choice->want = 0 - founder_want;
        choice->type = CT_CIVILIAN;
        choice->value.utype = founder_type; /* Default */
        choice->need_boat = TRUE;

        /* Then try to overwrite it with ferryboat information
         * If no ferryboat is found, above founder choice stays. */
        dai_choose_role_unit(ait, pplayer, pcity, choice, CT_CIVILIAN,
                             L_FERRYBOAT, -founder_want, TRUE);
        adv_choice_set_use(choice, "founder's boat");
      }
    } else if (!founder_type
               && (founder_want > choice->want || founder_want < -choice->want)) {
      /* Can't build founders. Lets stimulate science */
      dai_wants_role_unit(ait, pplayer, pcity,
                          action_id_get_role(ACTION_FOUND_CITY),
                          founder_want);
    }
  }

  {
    struct adv_choice *cur;

    /* Consider building caravan-type units to aid wonder construction */
    cur = adv_new_choice();
    adv_choice_set_use(cur, "wonder");
    dai_choose_help_wonder(ait, pcity, cur, adv);
    choice = adv_better_choice_free(choice, cur);

    /* Consider city improvements */
    cur = adv_new_choice();
    adv_choice_set_use(cur, "improvement");
    building_advisor_choose(pcity, cur);
    cur->want = cur->want * (0.5 + (ai_trait_get_value(TRAIT_BUILDER, pplayer)
                                    / TRAIT_DEFAULT_VALUE / 2));
    choice = adv_better_choice_free(choice, cur);

    /* Consider building caravan-type units for trade route */
    cur = adv_new_choice();
    adv_choice_set_use(cur, "trade route");
    dai_choose_trade_route(ait, pcity, cur, adv);
    choice = adv_better_choice_free(choice, cur);
  }

  /* Once domestic alternatives have been scored, impose the explainable
   * largepox sequence.  Founder selection is intentionally after ordinary
   * radius/economic buildings: it carries a larger want and must not be
   * accidentally overwritten while city slots remain.  A population ceiling
   * is the sole final override because neither a founder nor another building
   * can make a capped city grow. */
  dai_largepox_choose_radius_building(ait, pcity, choice);
  dai_largepox_choose_economic_chain(ait, pcity, choice);
  dai_largepox_choose_trade_handoff(ait, pplayer, pcity, choice);
  dai_largepox_choose_trade_prebuild(ait, pplayer, pcity, choice);
  dai_largepox_choose_founder(ait, pplayer, pcity, choice);
  dai_largepox_choose_worker(ait, pplayer, pcity, choice);
  dai_largepox_choose_growth_building(ait, pcity, choice);

  if (choice->want > DAI_WANT_DOMESTIC_MAX
      && !(dai_largepox_enabled()
           && choice->type == CT_CIVILIAN
           && choice->value.utype != nullptr
           && utype_is_cityfounder(choice->value.utype)
           && def_ai_city_data(pcity, ait)->urgency == 0)
      && !(dai_largepox_enabled()
           && choice->type == CT_CIVILIAN
           && choice->value.utype != nullptr
           && utype_can_do_action(choice->value.utype, ACTION_TRADE_ROUTE)
           && city_list_size(pplayer->cities) >= 9
           && def_ai_city_data(pcity, ait)->urgency == 0)
      && !(dai_largepox_enabled()
           && choice->type == CT_BUILDING
           && choice->value.building != nullptr
           && dai_largepox_is_economic_building(choice->value.building)
           && def_ai_city_data(pcity, ait)->urgency <= 1)) {
    /* If we don't do following, we buy caravans in city X when we should be
     * saving money to buy defenses for city Y. -- Syela */
    choice->want = DAI_WANT_DOMESTIC_MAX;
  }

  return choice;
}

/***********************************************************************//**
  Calculate walking distances to wonder city from nearby cities.
***************************************************************************/
void dai_wonder_city_distance(struct ai_type *ait, struct player *pplayer,
                              struct adv_data *adv)
{
  struct pf_map *pfm;
  struct pf_parameter parameter;
  struct unit_type *punittype;
  struct unit *ghost;
  int maxrange;
  struct city *wonder_city = game_city_by_number(adv->wonder_city);
  const struct civ_map *nmap = &(wld.map);

  city_list_iterate(pplayer->cities, acity) {
    /* Mark unavailable */
    def_ai_city_data(acity, ait)->distance_to_wonder_city = 0;
  } city_list_iterate_end;

  if (wonder_city == NULL) {
    return;
  }

  punittype = best_role_unit_for_player(pplayer,
      action_id_get_role(ACTION_HELP_WONDER));

  if (!punittype) {
    return;
  }

  fc_assert_msg(utype_can_do_action(punittype, ACTION_HELP_WONDER),
                "Non existence of wonder helper unit not caught");

  ghost = unit_virtual_create(pplayer, wonder_city, punittype, 0);
  maxrange = unit_move_rate(ghost) * 7;

  pft_fill_unit_parameter(&parameter, nmap, ghost);
  parameter.omniscience = !has_handicap(pplayer, H_MAP);
  pfm = pf_map_new(&parameter);

  pf_map_move_costs_iterate(pfm, ptile, move_cost, FALSE) {
    struct city *acity = tile_city(ptile);

    if (move_cost > maxrange) {
      break;
    }
    if (!acity) {
      continue;
    }
    if (city_owner(acity) == pplayer) {
      def_ai_city_data(acity, ait)->distance_to_wonder_city = move_cost;
    }
  } pf_map_move_costs_iterate_end;

  pf_map_destroy(pfm);
  unit_virtual_destroy(ghost);
}
