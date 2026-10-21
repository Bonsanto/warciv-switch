/***********************************************************************
 Freeciv - Copyright (C) 2002 - The Freeciv Project
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

/* common */
#include "city.h"
#include "game.h"
#include "government.h"
#include "map.h"
#include "multipliers.h"
#include "research.h"
#include "traderoutes.h"

/* aicore */
#include "aiactions.h"

/* server */
#include "cityturn.h"
#include "plrhand.h"

/* server/advisors */
#include "advdata.h"

/* ai/default */
#include "daicity.h"
#include "daidiplomacy.h"
#include "daieffects.h"
#include "daiferry.h"
#include "daiplayer.h"
#include "daisettler.h"
#include "daiunit.h"

#include "daidata.h"

#define DAI_LARGEPOX_ROUTE_COVERAGE_MIN 100
#define DAI_LARGEPOX_TRADE_NETWORK_MAX_TURNS 28

static void dai_diplomacy_new(struct ai_type *ait,
                              const struct player *plr1,
                              const struct player *plr2);
static void dai_diplomacy_defaults(struct ai_type *ait,
                                   const struct player *plr1,
                                   const struct player *plr2);
static void dai_diplomacy_destroy(struct ai_type *ait,
                                  const struct player *plr1,
                                  const struct player *plr2);

/************************************************************************//**
  Initialize ai data structure
****************************************************************************/
void dai_data_init(struct ai_type *ait, struct player *pplayer)
{
  struct ai_plr *ai = def_ai_player_data(pplayer, ait);

  ai->phase_initialized = FALSE;

  ai->last_num_continents = -1;
  ai->last_num_oceans = -1;

  ai->diplomacy.player_intel_slots
    = fc_calloc(player_slot_count(),
                sizeof(*ai->diplomacy.player_intel_slots));
  player_slots_iterate(pslot) {
    const struct ai_dip_intel **player_intel_slot
      = ai->diplomacy.player_intel_slots + player_slot_index(pslot);
    *player_intel_slot = NULL;
  } player_slots_iterate_end;

  players_iterate(aplayer) {
    /* create ai diplomacy states for all other players */
    dai_diplomacy_new(ait, pplayer, aplayer);
    dai_diplomacy_defaults(ait, pplayer, aplayer);
    /* create ai diplomacy state of this player */
    if (aplayer != pplayer) {
      dai_diplomacy_new(ait, aplayer, pplayer);
      dai_diplomacy_defaults(ait, aplayer, pplayer);
    }
  } players_iterate_end;

  ai->diplomacy.strategy = WIN_OPEN;
  ai->diplomacy.timer = 0;
  ai->diplomacy.love_coeff = 4; /* 4% */
  ai->diplomacy.love_incr = MAX_AI_LOVE * 3 / 100;
  ai->diplomacy.req_love_for_peace = MAX_AI_LOVE / 8;
  ai->diplomacy.req_love_for_alliance = MAX_AI_LOVE / 4;

  ai->settler = NULL;

  ai->largepox.state = DAI_GROWTH_BOOTSTRAP;
  ai->largepox.resume_state = DAI_GROWTH_BOOTSTRAP;
  ai->largepox.last_transition_turn = game.info.turn;
  ai->largepox.support_city_id = 0;
  ai->largepox.target_core_cities = 11;
  ai->largepox.core_cities = 0;
  ai->largepox.route_slots = 0;
  ai->largepox.established_routes = 0;
  ai->largepox.route_coverage_pct = 0;
  ai->largepox.rapture_eligible = 0;
  ai->largepox.rapture_ready = 0;
  ai->largepox.average_core_size = 0;
  ai->largepox.founder_stalled_turns = 0;
  ai->largepox.last_city_count = 0;
  ai->largepox.last_city_growth_turn = game.info.turn;

  /* Initialise autoworker. */
  dai_auto_settler_init(ai);
}

/************************************************************************//**
  Whether the optional Warclient largepox strategy is enabled.
****************************************************************************/
bool dai_largepox_enabled(void)
{
  return game.server.ai_largepox;
}

/************************************************************************//**
  Stable, untranslated name used in diagnostic output.
****************************************************************************/
const char *dai_largepox_state_name(enum dai_growth_state state)
{
  switch (state) {
  case DAI_GROWTH_BOOTSTRAP:
    return "bootstrap";
  case DAI_GROWTH_EXPAND_CORE:
    return "expand-core";
  case DAI_GROWTH_TRADE_NETWORK:
    return "trade-network";
  case DAI_GROWTH_RAPTURE_PREP:
    return "rapture-prep";
  case DAI_GROWTH_RAPTURE:
    return "rapture";
  case DAI_GROWTH_METROPOLIS:
    return "metropolis";
  case DAI_GROWTH_EMERGENCY:
    return "emergency";
  }

  return "unknown";
}

/************************************************************************//**
  Return the current largepox state for a default-AI player.
****************************************************************************/
enum dai_growth_state dai_largepox_state(struct ai_type *ait,
                                         const struct player *pplayer)
{
  return def_ai_player_data(pplayer, ait)->largepox.state;
}

/************************************************************************//**
  A support city builds founders, terrain improvers, and defenders but is not
  required to join the four-route rapture core.
****************************************************************************/
bool dai_largepox_city_is_support(struct ai_type *ait,
                                  const struct player *pplayer,
                                  const struct city *pcity)
{
  return dai_largepox_enabled()
         && def_ai_player_data(pplayer, ait)->largepox.support_city_id
            == pcity->id;
}

/************************************************************************//**
  Whether city and tax management should favor vertical growth.
****************************************************************************/
bool dai_largepox_growth_focus(struct ai_type *ait,
                               const struct player *pplayer)
{
  enum dai_growth_state state = dai_largepox_state(ait, pplayer);

  return dai_largepox_enabled()
         && (state == DAI_GROWTH_RAPTURE_PREP
             || state == DAI_GROWTH_RAPTURE
             || state == DAI_GROWTH_METROPOLIS
             /* The last coastal infill wave must not switch off the
              * established core's celebration merely because three cities
              * are still missing. */
             || (state == DAI_GROWTH_EXPAND_CORE
                 && city_list_size(pplayer->cities) >= 12));
}

/************************************************************************//**
  Return the active largepox expansion target.

  On a broad island, stop the opening wave at twelve cities.  Expose the
  complete map-sensitive target only after that core has both an average
  three established route slots per city and an average size of ten.
  This keeps the AI growing and building infrastructure instead of spending
  the first rapture cycle on low-value infill founders.
****************************************************************************/
int dai_largepox_total_city_target(struct ai_type *ait,
                                   const struct player *pplayer)
{
  const struct ai_plr *ai = def_ai_player_data(pplayer, ait);
  int complete_target = ai->largepox.target_core_cities + 1;
  int opening_target = MIN(12, complete_target);

  if (ai->largepox.established_routes < 3 * opening_target
      || ai->largepox.average_core_size < 10) {
    return opening_target;
  }

  return complete_target;
}

/************************************************************************//**
  Choose one non-capital city as a stable support city once the empire is
  large enough. Lower-yield, smaller cities are preferred.
****************************************************************************/
static void dai_largepox_select_support_city(struct ai_type *ait,
                                              struct player *pplayer)
{
  struct ai_plr *ai = def_ai_player_data(pplayer, ait);
  struct city *best = nullptr;
  int best_score = FC_INFINITY;
  bool current_valid = FALSE;
  int complete_target = ai->largepox.target_core_cities + 1;
  int opening_target = MIN(12, complete_target);

  /* A dedicated support city is useful while the core is being founded,
   * but keeping it forever costs one full set of trade routes and one
   * rapturing metropolis.  Once expansion has handed off to the trade
   * network, every city joins the economic core. */
  if (ai->largepox.state != DAI_GROWTH_BOOTSTRAP
      && ai->largepox.state != DAI_GROWTH_EXPAND_CORE) {
    ai->largepox.support_city_id = 0;
    return;
  }

  /* The support city is an opening accelerator, not a permanent exclusion.
   * In the second expansion wave it used to remove up to four routes from
   * the aggregate metric.  That dropped the active target back from fifteen
   * to twelve and made completed Settlers abandon their city mission. */
  if (city_list_size(pplayer->cities) >= opening_target) {
    ai->largepox.support_city_id = 0;
    return;
  }

  if (city_list_size(pplayer->cities) < 5) {
    ai->largepox.support_city_id = 0;
    return;
  }

  city_list_iterate(pplayer->cities, pcity) {
    if (pcity->id == ai->largepox.support_city_id) {
      current_valid = TRUE;
      break;
    }
  } city_list_iterate_end;

  if (current_valid) {
    return;
  }

  city_list_iterate(pplayer->cities, pcity) {
    int score;

    if (is_capital(pcity)) {
      continue;
    }

    score = city_size_get(pcity) * 5
            + MAX(0, pcity->surplus[O_FOOD]) * 3
            + MAX(0, pcity->surplus[O_SHIELD]) * 2
            + MAX(0, pcity->surplus[O_TRADE]) * 5
            + city_num_trade_routes(pcity) * 12;
    if (score < best_score
        || (score == best_score && best != nullptr && pcity->id < best->id)) {
      best = pcity;
      best_score = score;
    }
  } city_list_iterate_end;

  ai->largepox.support_city_id = best != nullptr ? best->id : 0;
}

/************************************************************************//**
  Set a map-sensitive target capped at the observed Warclient pattern of
  eleven growth cities plus one support city.  The previous thresholds made
  the AI stop at seven to ten cities on islands where a human largepox layout
  still had several useful coastal gaps.  Keep scaling down genuinely cramped
  starts, but use one more density step on ordinary 200-tile-per-player
  archipelagos.
****************************************************************************/
static int dai_largepox_target_core_cities(const struct player *pplayer,
                                            const struct adv_data *adv)
{
  int occupied_land = 0;

  for (Continent_id cont = 1; cont <= adv->num_continents; cont++) {
    if (adv->stats.cities[cont] > 0) {
      occupied_land += adv->continents[cont].size;
    }
  }

  if (occupied_land < 40) {
    return 7;
  } else if (occupied_land < 48) {
    return 8;
  } else if (occupied_land < 56) {
    return 9;
  } else if (occupied_land < 64) {
    return 10;
  }

  /* A broad fair-island start has room for the fifteen-city interlocking
   * layout used by the reference games.  Twelve was an opening checkpoint,
   * not a final density target; treating it as a hard cap left long coastal
   * strips permanently empty. */
  return 14;
}

/************************************************************************//**
  Refresh the explainable largepox state and its aggregate metrics.
****************************************************************************/
void dai_largepox_update(struct ai_type *ait, struct player *pplayer)
{
  struct ai_plr *ai;
  struct adv_data *adv;
  enum dai_growth_state old_state, desired;
  int total_size = 0;
  int severe_food_deficits = 0;
  int best_founder_want = 0;
  int city_count;
  int total_city_target;
  int opening_city_target;
  int state_age;
  bool mature_infill;
  bool trade_network_timed_out;

  if (!dai_largepox_enabled() || !is_ai(pplayer) || is_barbarian(pplayer)) {
    return;
  }

  ai = def_ai_player_data(pplayer, ait);
  adv = adv_data_get(pplayer, nullptr);
  old_state = ai->largepox.state;
  state_age = MAX(0, game.info.turn - ai->largepox.last_transition_turn);

  city_count = city_list_size(pplayer->cities);
  ai->largepox.target_core_cities
    = dai_largepox_target_core_cities(pplayer, adv);
  dai_largepox_select_support_city(ait, pplayer);

  if (city_count > ai->largepox.last_city_count) {
    ai->largepox.last_city_growth_turn = game.info.turn;
  }
  ai->largepox.last_city_count = city_count;

  total_city_target = ai->largepox.target_core_cities + 1;
  opening_city_target = MIN(12, total_city_target);
  ai->largepox.core_cities = 0;
  ai->largepox.route_slots = 0;
  ai->largepox.established_routes = 0;
  ai->largepox.rapture_eligible = 0;
  ai->largepox.rapture_ready = 0;

  city_list_iterate(pplayer->cities, pcity) {
    struct ai_city *city_data = def_ai_city_data(pcity, ait);

    best_founder_want = MAX(best_founder_want,
                            MAX(0, city_data->founder_want));
    if (pcity->surplus[O_FOOD] <= -2) {
      severe_food_deficits++;
    }
    if (dai_largepox_city_is_support(ait, pplayer, pcity)) {
      continue;
    }

    ai->largepox.core_cities++;
    total_size += city_size_get(pcity);
    ai->largepox.route_slots += max_trade_routes(pcity);
    ai->largepox.established_routes
      += MIN(city_num_trade_routes(pcity), max_trade_routes(pcity));

    /* Rapture is deliberately restricted to size three or greater. */
    if (city_size_get(pcity) >= 3
        && city_can_grow_to(pcity, city_size_get(pcity) + 1)) {
      ai->largepox.rapture_eligible++;
      if (city_happy(pcity) && pcity->surplus[O_FOOD] >= 1) {
        ai->largepox.rapture_ready++;
      }
    }
  } city_list_iterate_end;

  ai->largepox.average_core_size
    = ai->largepox.core_cities > 0
      ? total_size / ai->largepox.core_cities : 0;
  ai->largepox.route_coverage_pct
    = ai->largepox.route_slots > 0
      ? 100 * ai->largepox.established_routes / ai->largepox.route_slots
      : 100;

  if (ai->largepox.core_cities >= 9 && best_founder_want < 50) {
    ai->largepox.founder_stalled_turns++;
  } else {
    ai->largepox.founder_stalled_turns = 0;
  }

  trade_network_timed_out
    = old_state == DAI_GROWTH_TRADE_NETWORK
      && state_age >= DAI_LARGEPOX_TRADE_NETWORK_MAX_TURNS;
  mature_infill
    = city_count >= opening_city_target
      /* Match the active target above: finish the trade backbone and the
       * first vertical-growth cycle before spending production on the last
       * coastal infill sites. */
      && ai->largepox.established_routes >= 3 * opening_city_target
      && ai->largepox.average_core_size >= 10;

  if (severe_food_deficits > 0) {
    if (old_state != DAI_GROWTH_EMERGENCY) {
      ai->largepox.resume_state = old_state;
    }
    desired = DAI_GROWTH_EMERGENCY;
  } else if (old_state == DAI_GROWTH_EMERGENCY) {
    desired = ai->largepox.resume_state;
  } else if (city_count < 3) {
    desired = DAI_GROWTH_BOOTSTRAP;
  } else if (city_count < opening_city_target
             && best_founder_want >= 50) {
    /* Resume expansion whenever a legal useful site exists.  State-age and
     * stall timeouts made this transition effectively one-way: after the AI
     * began rapture, a newly explored coastal gap could remain empty for the
     * rest of the game.  Compare the complete empire size here, not only the
     * core count; the latter deliberately excludes the support city during
     * expansion and otherwise caused an off-by-one stop. */
    desired = DAI_GROWTH_EXPAND_CORE;
  } else if (city_count < total_city_target
             && mature_infill && best_founder_want >= 50) {
    /* Complete the high-density coastal infill only after the original
     * twelve-city core has funded its route network.  Expanding to fifteen
     * before caravans would merely move the old delay to a larger target. */
    desired = DAI_GROWTH_EXPAND_CORE;
  } else if (ai->largepox.core_cities < 5) {
    desired = DAI_GROWTH_EXPAND_CORE;
  } else if (ai->largepox.route_coverage_pct
             < DAI_LARGEPOX_ROUTE_COVERAGE_MIN
             && !trade_network_timed_out
             && (old_state == DAI_GROWTH_BOOTSTRAP
                 || old_state == DAI_GROWTH_EXPAND_CORE
                 || old_state == DAI_GROWTH_TRADE_NETWORK)) {
    /* A partial network is enough to fund the first rapture cycle. Do not
     * wait forever when pathfinding or geography leaves some route slots
     * temporarily unreachable; missing routes remain valuable after this
     * state. */
    desired = DAI_GROWTH_TRADE_NETWORK;
  } else if (ai->largepox.average_core_size >= 20) {
    desired = DAI_GROWTH_METROPOLIS;
  } else if (old_state == DAI_GROWTH_RAPTURE) {
    /* Keep trying instead of oscillating when a city temporarily loses
     * happiness or its food surplus. Tax management will reassess the
     * eligible core every turn. */
    desired = DAI_GROWTH_RAPTURE;
  } else if (ai->largepox.rapture_eligible > 0 && adv->celebrate) {
    desired = DAI_GROWTH_RAPTURE;
  } else {
    desired = DAI_GROWTH_RAPTURE_PREP;
  }

  /* Avoid oscillation while still allowing emergencies immediately. */
  if (desired != old_state
      && (desired == DAI_GROWTH_EMERGENCY
          || old_state == DAI_GROWTH_EMERGENCY
          || game.info.turn - ai->largepox.last_transition_turn >= 2)) {
    if (desired == DAI_GROWTH_EXPAND_CORE
        && old_state != DAI_GROWTH_BOOTSTRAP
        && old_state != DAI_GROWTH_EXPAND_CORE) {
      /* Record when the renewed expansion pass began.  Founding a city
       * refreshes this value naturally on the following update. */
      ai->largepox.last_city_growth_turn = game.info.turn;
    }
    ai->largepox.state = desired;
    ai->largepox.last_transition_turn = game.info.turn;
    log_verbose("%s largepox: %s -> %s; core=%d/%d, routes=%d/%d (%d%%), "
                "rapture=%d/%d, avg-size=%d, support=%d, state-age=%d",
                player_name(pplayer), dai_largepox_state_name(old_state),
                dai_largepox_state_name(desired), ai->largepox.core_cities,
                ai->largepox.target_core_cities,
                ai->largepox.established_routes, ai->largepox.route_slots,
                ai->largepox.route_coverage_pct, ai->largepox.rapture_ready,
                ai->largepox.rapture_eligible,
                ai->largepox.average_core_size,
                ai->largepox.support_city_id,
                state_age);
  }
}

/************************************************************************//**
  Deinitialize ai data structure
****************************************************************************/
void dai_data_close(struct ai_type *ait, struct player *pplayer)
{
  struct ai_plr *ai = def_ai_player_data(pplayer, ait);

  /* Finish the phase if it's open - free resources related to
   * open/finish cycle */
  dai_data_phase_finished(ait, pplayer);

  /* Free autoworker. */
  dai_auto_settler_free(ai);

  if (ai->diplomacy.player_intel_slots != NULL) {
    players_iterate(aplayer) {
      /* destroy the ai diplomacy states of this player with others ... */
      dai_diplomacy_destroy(ait, pplayer, aplayer);
      /* and of others with this player. */
      if (aplayer != pplayer) {
        dai_diplomacy_destroy(ait, aplayer, pplayer);
      }
    } players_iterate_end;
    free(ai->diplomacy.player_intel_slots);
  }
}

/************************************************************************//**
  Return whether data phase is currently open. Data phase is open
  between dai_data_phase_begin() and dai_data_phase_finished() calls.
****************************************************************************/
bool is_ai_data_phase_open(struct ai_type *ait, struct player *pplayer)
{
  struct ai_plr *ai = def_ai_player_data(pplayer, ait);

  return ai->phase_initialized;
}

/************************************************************************//**
  Make and cache lots of calculations needed for other functions.
****************************************************************************/
void dai_data_phase_begin(struct ai_type *ait, struct player *pplayer,
                          bool is_new_phase)
{
  struct ai_plr *ai = def_ai_player_data(pplayer, ait);
  bool caller_closes;

  /* Note that this refreshes advisor data if needed. ai_plr_data_get()
     is expected to refresh advisor data if needed, and ai_plr_data_get()
     depends on this call
     ai_plr_data_get()->ai_data_phase_begin()->adv_data_get() to do it.
     If you change this, you may need to adjust ai_plr_data_get() also. */
  struct adv_data *adv;

  if (ai->phase_initialized) {
    return;
  }

  ai->phase_initialized = TRUE;

  adv = adv_data_get(pplayer, &caller_closes);

  /* Store current number of known continents and oceans so we can compare
     against it later in order to see if ai data needs refreshing. */
  ai->last_num_continents = adv->num_continents;
  ai->last_num_oceans = adv->num_oceans;

  /*** Diplomacy ***/
  if (is_ai(pplayer) && !is_barbarian(pplayer) && is_new_phase) {
    dai_diplomacy_begin_new_phase(ait, pplayer);
  }

  /* Set per-player variables. We must set all players, since players
   * can be created during a turn, and we don't want those to have
   * invalid values. */
  players_iterate(aplayer) {
    struct ai_dip_intel *adip = dai_diplomacy_get(ait, pplayer, aplayer);

    adip->is_allied_with_enemy = NULL;
    adip->at_war_with_ally = NULL;
    adip->is_allied_with_ally = NULL;

    players_iterate(check_pl) {
      if (check_pl == pplayer
          || check_pl == aplayer
          || !check_pl->is_alive) {
        continue;
      }
      if (pplayers_allied(aplayer, check_pl)
          && player_diplstate_get(pplayer, check_pl)->type == DS_WAR) {
       adip->is_allied_with_enemy = check_pl;
      }
      if (pplayers_allied(pplayer, check_pl)
          && player_diplstate_get(aplayer, check_pl)->type == DS_WAR) {
        adip->at_war_with_ally = check_pl;
      }
      if (pplayers_allied(aplayer, check_pl)
          && pplayers_allied(pplayer, check_pl)) {
        adip->is_allied_with_ally = check_pl;
      }
    } players_iterate_end;
  } players_iterate_end;

  /*** Statistics ***/

  ai->stats.workers = fc_calloc(adv->num_continents + 1, sizeof(int));
  ai->stats.ocean_workers = fc_calloc(adv->num_oceans + 1, sizeof(int));
  unit_list_iterate(pplayer->units, punit) {
    struct tile *ptile = unit_tile(punit);

    if (unit_has_type_flag(punit, UTYF_WORKERS)) {
      Continent_id cont = tile_continent(ptile);

      if (is_ocean_tile(ptile)) {
        if (cont <= 0 && -cont <= adv->num_oceans) {
          ai->stats.ocean_workers[(int)-cont]++;
        } else {
          /* Always fails */
          fc_assert(cont <= 0 && -cont <= adv->num_oceans);
        }
      } else {
        if (cont >= 0 && cont <= adv->num_continents) {
          ai->stats.workers[(int)cont]++;
        } else {
          /* Always fails */
          fc_assert(cont >= 0 && cont <= adv->num_continents);
        }
      }
    }
  } unit_list_iterate_end;

  BV_CLR_ALL(ai->stats.diplomat_reservations);
  unit_list_iterate(pplayer->units, punit) {
    if (aia_utype_is_considered_spy_vs_city(unit_type_get(punit))
        && def_ai_unit_data(punit, ait)->task == AIUNIT_ATTACK) {

      fc_assert_msg(punit->goto_tile != NULL, "No target city for spy action");

      if (punit->goto_tile != NULL) {
        struct city *pcity = tile_city(punit->goto_tile);

        if (pcity != NULL) {
          /* Heading somewhere on a mission, reserve target. */
          BV_SET(ai->stats.diplomat_reservations, pcity->id);
        }
      }
    }
  } unit_list_iterate_end;

  aiferry_init_stats(ait, pplayer);

  /*** Interception engine ***/

  /* We are tracking a unit if punit->server.ai->cur_pos is not NULL. If we
   * are not tracking, start tracking by setting cur_pos. If we are,
   * fill prev_pos with previous cur_pos. This way we get the
   * necessary coordinates to calculate a probable trajectory. */
  players_iterate_alive(aplayer) {
    if (aplayer == pplayer) {
      continue;
    }
    unit_list_iterate(aplayer->units, punit) {
      struct unit_ai *unit_data = def_ai_unit_data(punit, ait);

      if (!unit_data->cur_pos) {
        /* Start tracking */
        unit_data->cur_pos = &unit_data->cur_struct;
        unit_data->prev_pos = NULL;
      } else {
        unit_data->prev_struct = unit_data->cur_struct;
        unit_data->prev_pos = &unit_data->prev_struct;
      }
      *unit_data->cur_pos = unit_tile(punit);
    } unit_list_iterate_end;
  } players_iterate_alive_end;

  if (caller_closes) {
    adv_data_phase_done(pplayer);
  }
}

/************************************************************************//**
  Clean up ai data after phase finished.
****************************************************************************/
void dai_data_phase_finished(struct ai_type *ait, struct player *pplayer)
{
  struct ai_plr *ai = def_ai_player_data(pplayer, ait);

  if (!ai->phase_initialized) {
    return;
  }

  free(ai->stats.workers);
  ai->stats.workers = NULL;

  free(ai->stats.ocean_workers);
  ai->stats.ocean_workers = NULL;

  ai->phase_initialized = FALSE;
}

/************************************************************************//**
  Get current default ai data related to player.
  If close is set, data phase will be opened even if it's currently closed,
  and the boolean will be set accordingly to tell caller that phase needs
  closing.
****************************************************************************/
struct ai_plr *dai_plr_data_get(struct ai_type *ait, struct player *pplayer,
                                bool *caller_closes)
{
  struct ai_plr *ai = def_ai_player_data(pplayer, ait);

  fc_assert_ret_val(ai != NULL, NULL);

  /* This assert really is required. See longer comment
     in adv_data_get() for equivalent code. */
#if defined(FREECIV_DEBUG) || defined(IS_DEVEL_VERSION)
  fc_assert(caller_closes != NULL || ai->phase_initialized);
#endif

  if (caller_closes != NULL) {
    *caller_closes = FALSE;
  }

  if (ai->last_num_continents != wld.map.num_continents
      || ai->last_num_oceans != wld.map.num_oceans) {
    /* We have discovered more continents, recalculate! */

    /* See adv_data_get() */
    if (ai->phase_initialized) {
      dai_data_phase_finished(ait, pplayer);
      dai_data_phase_begin(ait, pplayer, FALSE);
    } else {
      /* wrong order */
      log_debug("%s ai data phase closed when dai_plr_data_get() called",
                player_name(pplayer));
      dai_data_phase_begin(ait, pplayer, FALSE);
      if (caller_closes != NULL) {
        *caller_closes = TRUE;
      } else {
        dai_data_phase_finished(ait, pplayer);
      }
    }
  } else {
    if (!ai->phase_initialized && caller_closes != NULL) {
      dai_data_phase_begin(ait, pplayer, FALSE);
      *caller_closes = TRUE;
    }
  }

  return ai;
}

/************************************************************************//**
  Allocate new ai diplomacy slot
****************************************************************************/
static void dai_diplomacy_new(struct ai_type *ait,
                              const struct player *plr1,
                              const struct player *plr2)
{
  struct ai_dip_intel *player_intel;

  fc_assert_ret(plr1 != NULL);
  fc_assert_ret(plr2 != NULL);

  const struct ai_dip_intel **player_intel_slot
    = def_ai_player_data(plr1, ait)->diplomacy.player_intel_slots
      + player_index(plr2);

  fc_assert_ret(*player_intel_slot == NULL);

  player_intel = fc_calloc(1, sizeof(*player_intel));
  *player_intel_slot = player_intel;
}

/************************************************************************//**
  Set diplomacy data between two players to its default values.
****************************************************************************/
static void dai_diplomacy_defaults(struct ai_type *ait,
                                   const struct player *plr1,
                                   const struct player *plr2)
{
  struct ai_dip_intel *player_intel = dai_diplomacy_get(ait, plr1, plr2);

  fc_assert_ret(player_intel != NULL);

  /* pseudorandom value */
  player_intel->spam = (player_index(plr1) + player_index(plr2)) % 5;
  player_intel->countdown = -1;
  player_intel->war_reason = DAI_WR_NONE;
  player_intel->distance = 1;
  player_intel->ally_patience = 0;
  player_intel->asked_about_peace = 0;
  player_intel->asked_about_alliance = 0;
  player_intel->asked_about_ceasefire = 0;
  player_intel->warned_about_space = 0;
}

/************************************************************************//**
  Returns diplomatic state type between two players
****************************************************************************/
struct ai_dip_intel *dai_diplomacy_get(struct ai_type *ait,
                                       const struct player *plr1,
                                       const struct player *plr2)
{
  fc_assert_ret_val(plr1 != NULL, NULL);
  fc_assert_ret_val(plr2 != NULL, NULL);

  const struct ai_dip_intel **player_intel_slot
    = def_ai_player_data(plr1, ait)->diplomacy.player_intel_slots
      + player_index(plr2);

  fc_assert_ret_val(player_intel_slot != NULL, NULL);

  return (struct ai_dip_intel *) *player_intel_slot;
}

/************************************************************************//**
  Free resources allocated for diplomacy information between two players.
****************************************************************************/
static void dai_diplomacy_destroy(struct ai_type *ait,
                                  const struct player *plr1,
                                  const struct player *plr2)
{
  fc_assert_ret(plr1 != NULL);
  fc_assert_ret(plr2 != NULL);

  const struct ai_dip_intel **player_intel_slot
    = def_ai_player_data(plr1, ait)->diplomacy.player_intel_slots
      + player_index(plr2);

  if (*player_intel_slot != NULL) {
    free(dai_diplomacy_get(ait, plr1, plr2));
  }

  *player_intel_slot = NULL;
}

/************************************************************************//**
  Adjust multiplier values.
****************************************************************************/
void dai_adjust_policies(struct ai_type *ait, struct player *pplayer)
{
  bool needs_back_rearrange = FALSE;
  struct adv_data *adv;

  adv = adv_data_get(pplayer, NULL);

  multipliers_iterate(ppol) {
    if (multiplier_can_be_changed(ppol, pplayer)) {
      int orig_value = 0;
      int mp_val = player_multiplier_value(pplayer, ppol);
      int pidx = multiplier_index(ppol);
      bool better_found = FALSE;

      city_list_iterate(pplayer->cities, pcity) {
        orig_value += dai_city_want(pplayer, pcity, adv, NULL);
      } city_list_iterate_end;

      /* Consider reducing policy value */
      if (mp_val > ppol->start) {
        int new_value = 0;

        pplayer->multipliers[pidx].value = MAX(mp_val - ppol->step, ppol->start);

        city_list_iterate(pplayer->cities, acity) {
          auto_arrange_workers(acity);
        } city_list_iterate_end;

        city_list_iterate(pplayer->cities, pcity) {
          new_value += dai_city_want(pplayer, pcity, adv, NULL);
        } city_list_iterate_end;

        if (new_value > orig_value) {
          /* This is step to right direction, leave it in effect. */
          pplayer->multipliers[pidx].target = pplayer->multipliers[pidx].value;

          needs_back_rearrange = FALSE;
          better_found = TRUE;
        }
      }

      /* Consider increasing policy value */
      if (!better_found && mp_val < ppol->stop) {
        int new_value = 0;

        pplayer->multipliers[pidx].value = MIN(mp_val + ppol->step, ppol->stop);

        city_list_iterate(pplayer->cities, acity) {
          auto_arrange_workers(acity);
        } city_list_iterate_end;

        city_list_iterate(pplayer->cities, pcity) {
          new_value += dai_city_want(pplayer, pcity, adv, NULL);
        } city_list_iterate_end;

        if (new_value > orig_value) {
          /* This is step to right direction, leave it in effect. */
          pplayer->multipliers[pidx].target = pplayer->multipliers[pidx].value;

          needs_back_rearrange = FALSE;
          better_found = TRUE;
        }
      }

      if (!better_found) {
        /* Restore original multiplier value */
        pplayer->multipliers[pidx].value = mp_val;
        needs_back_rearrange = TRUE;
      }
    }
  } multipliers_iterate_end;

  if (needs_back_rearrange) {
    city_list_iterate(pplayer->cities, acity) {
      auto_arrange_workers(acity);
    } city_list_iterate_end;
  }
}

/************************************************************************//**
  Set value of the government.
****************************************************************************/
void dai_gov_value(struct ai_type *ait, struct player *pplayer,
                   struct government *gov, adv_want *val, bool *override)
{
  int dist;
  int bonus = 0; /* in percentage */
  int revolution_turns;
  struct universal source = { .kind = VUT_GOVERNMENT, .value.govern = gov };
  struct adv_data *adv;
  int turns = 9999; /* TODO: Set to correct value */
  int nplayers;
  const struct research *presearch;

  /* Use default handling of no-cities case */
  if (city_list_size(pplayer->cities) == 0) {
    *override = FALSE;
    return;
  }

  adv = adv_data_get(pplayer, NULL);
  nplayers = normal_player_count();
  presearch = research_get(pplayer);

  pplayer->government = gov;
  /* Ideally we should change tax rates here, but since
   * this is a rather big CPU operation, we'd rather not. */
  check_player_max_rates(pplayer);
  city_list_iterate(pplayer->cities, acity) {
    auto_arrange_workers(acity);
  } city_list_iterate_end;
  city_list_iterate(pplayer->cities, pcity) {
    bool capital;
    const struct req_context context = { .player = pplayer, .city = pcity };

    *val += dai_city_want(pplayer, pcity, adv, NULL);
    capital = is_capital(pcity);

    effect_list_iterate(get_req_source_effects(&source), peffect) {
      bool present = TRUE;
      bool active = TRUE;

      requirement_vector_iterate(&peffect->reqs, preq) {
        /* Check if all the requirements for the currently evaluated effect
         * are met, except for having the tech that we are evaluating.
         * TODO: Consider requirements that could be met later. */
        if (VUT_GOVERNMENT == preq->source.kind
            && preq->source.value.govern == gov) {
          present = preq->present;
          continue;
        }
        if (!is_req_active(&context, NULL, preq, RPT_POSSIBLE)) {
          active = FALSE;
          break; /* presence doesn't matter for inactive effects. */
        }

      } requirement_vector_iterate_end;

      if (active) {
        adv_want v1;

        v1 = dai_effect_value(pplayer, adv, pcity, capital,
                              turns, peffect, 1,
                              nplayers);

        if (!present) {
          /* Government removes the effect */
          *val -= v1;
        } else {
          *val += v1;
        }
      }
    } effect_list_iterate_end;
  } city_list_iterate_end;

  bonus += adv_gov_action_immunity_want(gov);
  bonus += adv_gov_player_bonus_want(pplayer);

  revolution_turns = get_player_bonus(pplayer, EFT_REVOLUTION_UNHAPPINESS);
  if (revolution_turns > 0) {
    bonus -= 6 / revolution_turns;
  }

  *val += (*val * bonus) / 100;

  /* FIXME: handle reqs other than technologies. */
  dist = 0;
  requirement_vector_iterate(&gov->reqs, preq) {
    if (VUT_ADVANCE == preq->source.kind) {
      dist += MAX(1, research_goal_unknown_techs(presearch,
                                                 advance_number(preq->source.value.advance)));
    }
  } requirement_vector_iterate_end;
  *val = amortize(*val, dist);

  *override = TRUE;
}
