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

/* utility */
#include "distribute.h"
#include "log.h"
#include "shared.h"
#include "timing.h"

/* common */
#include "city.h"
#include "game.h"
#include "government.h"
#include "improvement.h"
#include "map.h"
#include "nation.h"
#include "packets.h"
#include "player.h"
#include "research.h"
#include "specialist.h"
#include "tech.h"
#include "unit.h"
#include "victory.h"

/* common/aicore */
#include "cm.h"

/* server */
#include "citytools.h"
#include "cityturn.h"
#include "plrhand.h"
#include "sernet.h"
#include "spacerace.h"
#include "srv_log.h"
#include "unithand.h"

/* server/advisors */
#include "advdata.h"
#include "advspace.h"
#include "advtools.h"

/* ai */
#include "handicaps.h"

/* ai/default */
#include "daicity.h"
#include "daidata.h"
#include "daidiplomacy.h"
#include "dailog.h"
#include "daimilitary.h"
#include "daiplayer.h"
#include "daitech.h"
#include "daitools.h"

#include "daihand.h"

/*****************************************************************************
  A man builds a city
  With banks and cathedrals
  A man melts the sand so he can
  See the world outside
  A man makes a car
  And builds a road to run them on
  A man dreams of leaving
  but he always stays behind
  And these are the days when our work has come assunder
  And these are the days when we look for something other
  /U2 Lemon.
*****************************************************************************/

#define LOGLEVEL_TAX LOG_DEBUG

/* When setting rates, we accept negative balance if we have a lot of
 * gold/bulb reserves. This is how long time gold/bulb reserves should last.
 * For city grow due to rapture turns_for_rapture is added. */
#define AI_GOLD_RESERVE_MIN_TURNS 10
#define AI_BULBS_RESERVE_MIN_TURNS 10

/* This factor is used for the delta between the estimated and real income to
 * get 'real' rates for the AI player. */
#define PCT_DELTA_TAX 50
#define PCT_DELTA_SCI 10

/*************************************************************************//**
  Test the current temporary rates against the city-manager parameters.
  Largepox excludes its support city and size-one settlements from the
  happiness target. Size-two cities and cities temporarily blocked at a
  growth-building limit still count as ready when the city manager can make
  them happy with +1 food. They do not rapture yet, but keeping them prepared
  avoids lowering luxury immediately before they become eligible.
*****************************************************************************/
static void dai_evaluate_celebration(struct ai_type *ait,
                                     struct player *pplayer,
                                     const struct cm_parameter *cmp,
                                     bool largepox,
                                     int *can_celebrate,
                                     int *total_cities)
{
  *can_celebrate = 0;
  *total_cities = 0;

  city_list_iterate(pplayer->cities, pcity) {
    struct cm_result *cmr = cm_result_new(pcity);
    struct ai_city *city_data = def_ai_city_data(pcity, ait);
    struct cm_parameter city_cmp;

    city_data->celebrate = FALSE;
    if (largepox
        && (dai_largepox_city_is_support(ait, pplayer, pcity)
            || city_size_get(pcity) < 2)) {
      cm_result_destroy(cmr);
      continue;
    }

    cm_copy_parameter(&city_cmp, cmp);
    if (largepox && city_size_get(pcity) == 2
        && def_ai_player_data(pplayer, ait)->largepox.route_coverage_pct
           >= 50) {
      /* Size two cannot rapture yet.  Merely asking for +1 food can leave a
       * newly founded city at size two for most of the opening while its
       * full granary waits for one food per turn.  Keep it happy, but fill
       * the last part of its granary as fast as its tiles permit; on reaching
       * size three the normal happy +1-food governor takes over. */
      city_cmp.max_growth = TRUE;
    }

    cm_clear_cache(pcity);
    cm_query_result(pcity, &city_cmp, cmr, FALSE);
    (*total_cities)++;

    if (cmr->found_a_valid
        && (largepox ? cmr->surplus[O_FOOD] > 0
                     : pcity->surplus[O_FOOD] > 0)) {
      (*can_celebrate)++;
      /* This flag means "apply the happy +1-food allocation". The game
       * itself still restricts actual rapture growth to celebratesize and
       * city_can_grow_to(). Keeping size-two and temporarily capped cities
       * prepared is intentional. */
      city_data->celebrate = TRUE;
    } else if (largepox) {
      CITY_LOG(LOGLEVEL_TAX, pcity,
               "largepox celebration check failed at luxury=%d: "
               "valid=%d food=%d",
               pplayer->economic.luxury, cmr->found_a_valid,
               cmr->found_a_valid ? cmr->surplus[O_FOOD] : 0);
    }
    cm_result_destroy(cmr);
  } city_list_iterate_end;
}

/*************************************************************************//**
  Apply one previously selected celebration allocation against the tiles
  which remain after earlier cities in the same empire-wide pass.
*****************************************************************************/
static void dai_apply_celebration_city(struct ai_type *ait,
                                       const struct civ_map *nmap,
                                       struct city *pcity,
                                       const struct cm_parameter *cmp)
{
  struct cm_result *cmr = cm_result_new(pcity);
  struct cm_parameter city_cmp;

  log_base(LOGLEVEL_TAX, "setting %s to celebrate", city_name_get(pcity));
  cm_copy_parameter(&city_cmp, cmp);
  if (city_size_get(pcity) == 2
      && def_ai_player_data(city_owner(pcity), ait)
           ->largepox.route_coverage_pct >= 50) {
    city_cmp.max_growth = TRUE;
  }
  cm_clear_cache(pcity);
  cm_query_result(pcity, &city_cmp, cmr, FALSE);
  if (cmr->found_a_valid) {
    apply_cmresult_to_city(pcity, cmr);
    city_refresh_from_main_map(nmap, pcity, nullptr);
    if (!city_happy(pcity) || pcity->surplus[O_FOOD] < 1
        || pcity->surplus[O_SHIELD] < 1) {
      CITY_LOG(LOGLEVEL_TAX, pcity,
               "lost happy +1-food/+1-production state after applying "
               "governor");
    }
  } else {
    /* Shared city-map tiles may have been claimed earlier in this pass. */
    def_ai_city_data(pcity, ait)->celebrate = FALSE;
    CITY_LOG(LOGLEVEL_TAX, pcity,
             "cannot celebrate after neighbouring assignments");
  }
  cm_result_destroy(cmr);
}

/*************************************************************************//**
  Release the non-free workers of the largepox core before assigning its
  celebration layout.  Querying and applying one city at a time otherwise
  makes the result depend on the old owner of every shared tile: a neighbour
  can keep a food tile which is merely convenient for it but indispensable
  to a constrained coastal city.

  Convert every released worker to the default specialist temporarily so the
  city remains internally sane until its cm_result is applied.  City centres
  and other free-worked tiles are deliberately left alone.
*****************************************************************************/
static void dai_release_celebration_workers(const struct civ_map *nmap,
                                            struct city **cities,
                                            int city_count)
{
  for (int i = 0; i < city_count; i++) {
    struct city *pcity = cities[i];

    city_tile_iterate_skip_free_worked(nmap,
                                       city_map_radius_sq_get(pcity),
                                       city_tile(pcity), ptile,
                                       _index, _x, _y) {
      if (tile_worked(ptile) == pcity) {
        city_map_update_empty(pcity, ptile);
        pcity->specialists[DEFAULT_SPECIALIST]++;
      }
    } city_tile_iterate_skip_free_worked_end;

    city_refresh_from_main_map(nmap, pcity, nullptr);
    cm_clear_cache(pcity);
  }
}

/*************************************************************************//**
  Handle spaceship related stuff
*****************************************************************************/
static void dai_manage_spaceship(struct player *pplayer)
{
  if (victory_enabled(VC_SPACERACE)) {
    if (pplayer->spaceship.state == SSHIP_STARTED) {
      adv_spaceship_autoplace(pplayer, &pplayer->spaceship);

      /* if we have built the best possible spaceship  -- AJS 19990610 */
      if ((pplayer->spaceship.structurals == NUM_SS_STRUCTURALS)
          && (pplayer->spaceship.components == NUM_SS_COMPONENTS)
          && (pplayer->spaceship.modules == NUM_SS_MODULES))
        handle_spaceship_launch(pplayer);
    }
  }
}

/*************************************************************************//**
  Returns the total amount of trade generated (trade), total amount of
  gold needed as upkeep (expenses), and total amount of gold gained (income).
*****************************************************************************/
void dai_calc_data(const struct player *pplayer, int *trade, int *expenses,
                   int *income)
{
  if (trade != nullptr) {
    *trade = 0;
  }
  if (expenses != nullptr) {
    *expenses = 0;
  }
  if (income != nullptr) {
    *income = 0;
  }

  /* Find total trade surplus, gold expenses, and gold income */
  city_list_iterate(pplayer->cities, pcity) {
    if (trade != nullptr) {
      *trade += pcity->surplus[O_TRADE];
    }

    if (expenses != nullptr) {
      *expenses += pcity->usage[O_GOLD];
    }

    if (income != nullptr) {
      /* Also the immediately used gold is part of income, not only surplus */
      *income += pcity->usage[O_GOLD] + pcity->surplus[O_GOLD];
    }
  } city_list_iterate_end;

  switch (game.info.gold_upkeep_style) {
  case GOLD_UPKEEP_CITY:
    break;
  case GOLD_UPKEEP_MIXED:
  case GOLD_UPKEEP_NATION:
    /* Account for units with gold upkeep paid for by the nation. */
    unit_list_iterate(pplayer->units, punit) {
      *expenses += punit->upkeep[O_GOLD];
    } unit_list_iterate_end;
    break;
  }
}

/*****************************************************************************
  Additional data needed for the AI rates calculation
*****************************************************************************/
enum {
  AI_RATE_SCI = 0,
  AI_RATE_TAX,
  AI_RATE_LUX,
  AI_RATE_COUNT
};

enum celebration {
  AI_CELEBRATION_UNCHECKED,
  AI_CELEBRATION_NO,
  AI_CELEBRATION_YES,
};

/************************************************************************//**
  Whether the first route windfalls have finished capitalizing the core.

  Route-arrival bulbs carry the reference opening through this interval, so
  ordinary trade should be gold-first until the buildings which compound
  both luxury/tax and research exist throughout the core.  Switching to
  science immediately after Harbour left most cities without Stock Exchange,
  broke their 20% luxury celebration, and spent production on emergency
  Temples and Colosseums instead of the economic chain.
****************************************************************************/
static bool dai_largepox_gold_foundation_ready(struct ai_type *ait,
                                                const struct player *pplayer)
{
  static const char *const capitalization[] = {
    "Marketplace", "Aqueduct", "Bank", "Harbour",
    "Stock Exchange", "Library"
  };
  int core_cities = 0;

  city_list_iterate(pplayer->cities, pcity) {
    if (dai_largepox_city_is_support(ait, pplayer, pcity)) {
      continue;
    }

    core_cities++;
    for (size_t i = 0; i < ARRAY_SIZE(capitalization); i++) {
      const struct impr_type *building
        = improvement_by_rule_name(capitalization[i]);

      if (building != nullptr && !city_has_building(pcity, building)) {
        return FALSE;
      }
    }
  } city_list_iterate_end;

  return core_cities > 0;
}

/************************************************************************//**
  Whether mature-but-not-yet-capitalized cities need one luxury-rate step
  above the independent city-manager result.

  The rate search evaluates cities separately, while the real largepox pass
  must divide overlapping food and trade tiles among the complete dense core.
  At small sizes that distinction is harmless.  From average size eight on,
  the nominal minimum can make every isolated query happy yet lose several
  celebrations after shared tiles are assigned.  Keep a 10-point buffer only
  until Stock Exchange and Library complete the core's capitalization.
****************************************************************************/
static bool dai_largepox_needs_luxury_buffer(struct ai_type *ait,
                                              const struct player *pplayer)
{
  int core_cities = 0;
  int core_population = 0;

  if (dai_largepox_gold_foundation_ready(ait, pplayer)) {
    return FALSE;
  }

  city_list_iterate(pplayer->cities, pcity) {
    if (!dai_largepox_city_is_support(ait, pplayer, pcity)
        && city_size_get(pcity) >= 2) {
      core_cities++;
      core_population += city_size_get(pcity);
    }
  } city_list_iterate_end;

  return core_cities > 0 && core_population >= 8 * core_cities;
}

#define RATE_NOT_SET -1
#define RATE_VALID(_rate)                                                    \
  (_rate != RATE_NOT_SET)
#define RATE_REMAINS(_rates)                                                 \
  MAX(0, 100 - _rates[AI_RATE_SCI] - _rates[AI_RATE_TAX]                     \
         - _rates[AI_RATE_LUX])

/*************************************************************************//**
  Set tax/science/luxury rates.

  TODO: Add general support for luxuries: select the luxury rate at which
  all cities are content and the trade output (minus what is consumed by
  luxuries) is maximal. For this we need some more information from
  the city management code.

  TODO: Audit the use of pplayer->ai.maxbuycost in the code elsewhere,
  then add support for it here.

  This function first determines the minimum tax rate (rate_tax_min) and a tax
  rate for a balanced treasury (rate_tax_balance). Similarly, the science
  rates are determiend (rate_sci_min and rate_sci_balance). Considering the
  minimum rates for tax and science the chance for celebrations is checked. If
  celebration is possible for more than half of the cities, the needed luxury
  rate is saved as rate_lux_min_celebrate. For celebration some reserves are
  defined (see turns_for_rapture).

  At the end the results are compared and the rates are selected as:

  1. Go for celebration.
  2. Use a balanced science and treasury.
  3. go for a balaned treasury / science.
  4. Try to select the best (risk of tech loss or have to sell something).

  At the end the remaining is divided on science/gold/luxury depending on the
  AI settings.
*****************************************************************************/
static void dai_manage_taxes(struct ai_type *ait, struct player *pplayer)
{
  int maxrate = (has_handicap(pplayer, H_RATES)
                 ? get_player_bonus(pplayer, EFT_MAX_RATES) : 100);
  struct research *research = research_get(pplayer);
  enum celebration celebrate = AI_CELEBRATION_UNCHECKED;
  struct adv_data *adv = adv_data_get(pplayer, nullptr);
  int can_celebrate = 0, total_cities = 0;
  int trade = 0;         /* total amount of trade generated */
  int expenses = 0;      /* total amount of gold upkeep */
  int income = 0;        /* total amount of gold income */
  int turns_for_rapture; /* additional reserve needed for rapture */
  unsigned rates_save[AI_RATE_COUNT], rates_tmp[AI_RATE_COUNT];
  int rates[AI_RATE_COUNT];
  int result[AI_RATE_COUNT];
  int rate_tax_min = RATE_NOT_SET;
  int rate_tax_balance = RATE_NOT_SET;
  int rate_sci_min = RATE_NOT_SET;
  int rate_sci_balance = RATE_NOT_SET;
  int rate_lux_min_celebrate = maxrate;
  int delta_tax = 0, delta_sci = 0;
  int largepox_science_floor = 0;
  const struct civ_map *nmap = &(wld.map);
  bool largepox = dai_largepox_enabled();

#ifdef DEBUG_TIMERS
  struct timer *taxtimer = nullptr;
#endif

  struct cm_parameter cmp;

  if (!game.info.changable_tax) {
    return; /* This ruleset does not support changing tax rates. */
  }

  maxrate = CLIP(34, maxrate, 100);

  if (government_of_player(pplayer) == game.government_during_revolution) {
    return; /* This government does not support changing tax rates. */
  }

#ifdef DEBUG_TIMERS
  taxtimer = timer_new(TIMER_CPU, TIMER_DEBUG, "AI tax");
  timer_start(taxtimer);
#endif /* DEBUG_TIMERS */

  /* City parameters needed for celebrations. */
  cm_init_parameter(&cmp);
  cmp.require_happy = TRUE;    /* Note this one */
  cmp.allow_disorder = FALSE;
  cmp.allow_specialists = TRUE;
  cmp.factor[O_FOOD] = 20;
  cmp.minimal_surplus[O_GOLD] = -FC_INFINITY;
  if (largepox) {
    /* The Warclient governor contract is happiness with +1 food and at least
     * +1 production. Requiring +2 food made dense coastal cities abandon
     * their high-trade tiles, and the tax solver then needed 70--90% luxury
     * where the reference human layout used 20%. Shared tiles are resolved
     * again in the empire-wide apply pass. */
    cmp.minimal_surplus[O_FOOD] = 1;
    cmp.minimal_surplus[O_SHIELD] = 1;
    cmp.factor[O_FOOD] = 2;
    cmp.factor[O_SHIELD] = 12;
    cmp.factor[O_TRADE] = 5;
    cmp.factor[O_GOLD] = 25;
    cmp.factor[O_SCIENCE] = 3;
  }

  /* Define reserve (gold/bulbs) needed for rapture (celebration). */
  turns_for_rapture = adv->celebrate ? 0 : (game.info.rapturedelay + 1) * 3;

  log_base(LOGLEVEL_TAX, "%s [max] rate=%d", player_name(pplayer), maxrate);

  /* Save AI rates. */
  rates_save[AI_RATE_SCI] = pplayer->economic.science;
  rates_save[AI_RATE_TAX] = pplayer->economic.tax;
  rates_save[AI_RATE_LUX] = pplayer->economic.luxury;

  log_base(LOGLEVEL_TAX, "%s [old] (Sci/Lux/Tax)=%d/%d/%d",
           player_name(pplayer), rates_save[AI_RATE_SCI],
           rates_save[AI_RATE_LUX], rates_save[AI_RATE_TAX]);

  /* Get some data for the AI player. */
  dai_calc_data(pplayer, &trade, &expenses, &income);

  /* Get the estimates for tax with the current rates. */
  distribute(trade, AI_RATE_COUNT, rates_save, result);

  /* The delta between the estimate and the real value. */
  delta_tax = (result[AI_RATE_TAX] - expenses) - income;

  log_base(LOGLEVEL_TAX, "%s [tax] estimated=%d real=%d (delta=%d)",
           player_name(pplayer), result[AI_RATE_TAX] - expenses, income,
           delta_tax);

  /* Find minimum tax rate which gives us a positive balance for gold and
   * science. We assume that we want science most and luxuries least here,
   * and reverse or modify this assumption later.
   * Furthermore, We assume our entire civilization is one big city, and
   * distribute total income accordingly. This is a simplification that speeds
   * up the code significantly. */

  /* === Gold === */

  /* First set tax (gold) to the minimal available number */
  rates[AI_RATE_SCI] = maxrate; /* Assume we want science here */
  rates[AI_RATE_TAX] = MAX(0, 100 - maxrate * 2); /* If maxrate < 50% */
  rates[AI_RATE_LUX] = (100 - rates[AI_RATE_SCI] - rates[AI_RATE_TAX]);

  /* Now find the minimum tax with positive gold balance
   * Negative balance is acceptable if we have a lot of gold. */

  log_base(LOGLEVEL_TAX, "%s [tax] trade=%d gold=%d expenses=%d",
           player_name(pplayer), trade, pplayer->economic.gold, expenses);

  while (rates[AI_RATE_TAX] <= maxrate
         && rates[AI_RATE_SCI] >= 0
         && rates[AI_RATE_LUX] >= 0) {
    bool refill_coffers = pplayer->economic.gold < dai_gold_reserve(pplayer);
    int balance_tax, balance_tax_min;
    int i;

    for (i = 0; i < AI_RATE_COUNT; i++) {
      fc_assert(rates[i] >= 0);
      rates_tmp[i] = rates[i];
    }
    distribute(trade, AI_RATE_COUNT, rates_tmp, result);

    /* Consider the delta between the result and the real value from the
     * last turn to get better estimates. */
    balance_tax = (result[AI_RATE_TAX] - expenses)
                  - delta_tax * PCT_DELTA_TAX / 100;
    balance_tax_min = -(pplayer->economic.gold
                        / (AI_GOLD_RESERVE_MIN_TURNS + turns_for_rapture));

    log_base(LOGLEVEL_TAX, "%s [tax] Tax=%d income=%d",
             player_name(pplayer), rates[AI_RATE_TAX], balance_tax);

    if (!RATE_VALID(rate_tax_min) && balance_tax > balance_tax_min) {
      /* Just slightly negative; we can afford that for a while */
      rate_tax_min = rates[AI_RATE_TAX];
    }

    if (expenses == 0 || balance_tax > 0) {
      /* Ok, got no expenses or positive balance */
      if (refill_coffers && rates[AI_RATE_TAX] < maxrate) {
        /* Need to refill coffers, increase tax a bit */
        rates[AI_RATE_TAX] += 10;
        if (rates[AI_RATE_LUX] > 0) {
          rates[AI_RATE_LUX] -= 10;
        } else {
          rates[AI_RATE_SCI] -= 10;
        }

        log_base(LOGLEVEL_TAX, "%s [tax] Tax=%d to refill coffers",
                 player_name(pplayer), rates[AI_RATE_TAX]);
      }

      /* This is the minimum rate for a balanced treasury (gold). */
      rate_tax_balance = rates[AI_RATE_TAX];

      /* Done! Break the while loop */
      break;
    }

    /* Negative balance. Unacceptable - increase tax. */
    rates[AI_RATE_TAX] += 10;
    if (rates[AI_RATE_LUX] > 0) {
      rates[AI_RATE_LUX] -= 10;
    } else {
      rates[AI_RATE_SCI] -= 10;
    }
  }

  /* If no minimum value was found for the tax use the maximum tax rate. */
  rate_tax_min = RATE_VALID(rate_tax_min) ? rate_tax_min : maxrate;

  log_base(LOGLEVEL_TAX, "%s [tax] min=%d balanced=%d", player_name(pplayer),
           rate_tax_min, rate_tax_balance);

  /* === Science === */

  if (game.info.tech_upkeep_style != TECH_UPKEEP_NONE) {
    /* Tech upkeep activated. */
    int tech_upkeep = player_tech_upkeep(pplayer);
    int bulbs_researched = research->bulbs_researched;

    /* The delta between the estimate and the real value. */
    delta_sci = (result[AI_RATE_SCI] - tech_upkeep)
                - pplayer->server.bulbs_last_turn;
    log_base(LOGLEVEL_TAX, "%s [sci] estimated=%d real=%d (delta=%d)",
             player_name(pplayer),
             result[AI_RATE_SCI] - tech_upkeep,
             pplayer->server.bulbs_last_turn, delta_sci);

    log_base(LOGLEVEL_TAX, "%s [sci] trade=%d bulbs=%d upkeep=%d",
             player_name(pplayer), trade, research->bulbs_researched,
             tech_upkeep);

    rates[AI_RATE_TAX] = maxrate; /* Assume we want gold here */
    rates[AI_RATE_SCI] = MAX(0, 100 - maxrate * 2);
    rates[AI_RATE_LUX] = (100 - rates[AI_RATE_SCI] - rates[AI_RATE_TAX]);

    /* Now find the minimum science tax with positive bulbs balance
     * Negative balance is acceptable if we have a lots of bulbs. */
    while (rates[AI_RATE_SCI] <= maxrate
           && rates[AI_RATE_TAX] >= 0
           && rates[AI_RATE_LUX] >= 0) {
      int balance_sci, balance_sci_min;
      int i;

      for (i = 0; i < AI_RATE_COUNT; i++) {
        fc_assert(rates[i] >= 0);
        rates_tmp[i] = rates[i];
      }
      distribute(trade, AI_RATE_COUNT, rates_tmp, result);

      /* Consider the delta between the result and the real value from the
       * last turn. */
      balance_sci = (result[AI_RATE_SCI] - tech_upkeep)
                    - delta_sci * PCT_DELTA_SCI / 100;
      balance_sci_min = -(bulbs_researched
                          / (AI_BULBS_RESERVE_MIN_TURNS + turns_for_rapture));
      log_base(LOGLEVEL_TAX, "%s [sci] Sci=%d research=%d",
               player_name(pplayer), rates[AI_RATE_SCI], balance_sci);

      if (!RATE_VALID(rate_sci_min) && balance_sci > balance_sci_min) {
        /* Just slightly negative, if we can afford that for a while */
        rate_sci_min = rates[AI_RATE_SCI];
      }

      if (tech_upkeep == 0 || balance_sci > 0) {
        /* Ok, got no expenses or positive balance */
        rate_sci_balance = rates[AI_RATE_SCI];

        /* Done! Break the while loop */
        break;
      }

      /* Negative balance. Unacceptable - increase science.*/
      rates[AI_RATE_SCI] += 10;
      if (rates[AI_RATE_LUX] > 0) {
        rates[AI_RATE_LUX] -= 10;
      } else {
        rates[AI_RATE_TAX] -= 10;
      }
    }

    /* If no minimum value was found for the science use the maximum science
     * rate. */
    rate_sci_min = RATE_VALID(rate_sci_min) ? rate_sci_min : maxrate;

    log_base(LOGLEVEL_TAX, "%s [sci] min=%d balanced=%d",
             player_name(pplayer), rate_sci_min, rate_sci_balance);
  } else {
    /* No tech upkeep - minimum science value is 0. */
    rate_sci_min = 0;
  }

  if (largepox) {
    const struct advance *trade_advance = advance_by_rule_name("Trade");
    bool trade_known
      = trade_advance != nullptr
        && research_invention_state(research, advance_number(trade_advance))
           == TECH_KNOWN;

    /* Before Trade, research is the only route to the opening accelerator.
     * Afterwards its one-time bulbs carry research while ordinary trade is
     * converted into Marketplace/Aqueduct/Bank/Harbour.  Restore the 40%
     * strategic floor only when that gold foundation is complete, so the
     * mature empire still reaches Engineers, Railroad and Farmland. */
    largepox_science_floor
      = trade_known && !dai_largepox_gold_foundation_ready(ait, pplayer)
        ? 0 : MIN(maxrate, 40);

    /* Warclient's gold-first governor turns citizens left after securing
     * happiness and +1 food into tax collectors.  Once the economic
     * foundation is complete and research resumes, scientists are again the
     * useful fallback. */
    cmp.specialist_output
      = largepox_science_floor == 0 ? O_GOLD : O_SCIENCE;
  }

  /* === Luxury === */

  /* Should (and can) we celebrate? */
  /* TODO: In the future, we should check if we should
   * celebrate for other reasons than growth. Currently
   * this is ignored. Maybe we need ruleset AI hints. */
  /* TODO: Allow celebrate individual cities? No modpacks use this yet. */
  if (get_player_bonus(pplayer, EFT_RAPTURE_GROW) > 0
      && !has_handicap(pplayer, H_AWAY)
      && 100 > rate_tax_min + rate_sci_min) {
    celebrate = AI_CELEBRATION_NO;

    /* Set the minimum tax for a positive science and gold balance and use the
     * remaining trade goods for luxuries (max. luxuries). */
    if (largepox) {
      /* Find the lowest luxury rate at which every eligible core city can
       * celebrate.  Stopping at a merely successful majority makes a
       * largepox empire gain only a few citizens per turn and fall far behind
       * a human who keeps the complete core in rapture.  If terrain or a
       * missing growth building makes unanimity impossible, retain the best
       * available subset instead of turning rapture off for everybody. */
      int best_can_celebrate = 0;
      int best_luxury = -1;

      for (int lux = 0; lux <= maxrate; lux += 10) {
        rates[AI_RATE_SCI] = MAX(rate_sci_min, largepox_science_floor);
        rates[AI_RATE_LUX] = lux;
        rates[AI_RATE_TAX]
          = MIN(maxrate, 100 - rates[AI_RATE_SCI] - rates[AI_RATE_LUX]);

        if (rates[AI_RATE_TAX] < rate_tax_min) {
          continue;
        }
        rates[AI_RATE_SCI]
          = MIN(maxrate, rates[AI_RATE_SCI] + RATE_REMAINS(rates));
        if (RATE_REMAINS(rates) != 0) {
          continue;
        }

        pplayer->economic.luxury = rates[AI_RATE_LUX];
        pplayer->economic.tax = rates[AI_RATE_TAX];
        pplayer->economic.science = rates[AI_RATE_SCI];
        dai_evaluate_celebration(ait, pplayer, &cmp, TRUE,
                                 &can_celebrate, &total_cities);

        if (can_celebrate > best_can_celebrate) {
          best_can_celebrate = can_celebrate;
          best_luxury = lux;
        }
        if (total_cities > 0 && can_celebrate == total_cities) {
          best_luxury = lux;
          if (dai_largepox_needs_luxury_buffer(ait, pplayer)) {
            best_luxury = MIN(maxrate, best_luxury + 10);
          }
          break;
        }
      }

      if (best_luxury >= 0) {
        rates[AI_RATE_SCI] = MAX(rate_sci_min, largepox_science_floor);
        rates[AI_RATE_LUX] = best_luxury;
        rates[AI_RATE_TAX]
          = MAX(0, MIN(maxrate,
                       100 - rates[AI_RATE_SCI] - rates[AI_RATE_LUX]));
        rates[AI_RATE_SCI]
          = MIN(maxrate, rates[AI_RATE_SCI] + RATE_REMAINS(rates));

        pplayer->economic.luxury = rates[AI_RATE_LUX];
        pplayer->economic.tax = rates[AI_RATE_TAX];
        pplayer->economic.science = rates[AI_RATE_SCI];
        dai_evaluate_celebration(ait, pplayer, &cmp, TRUE,
                                 &can_celebrate, &total_cities);
        celebrate = AI_CELEBRATION_YES;
        rate_lux_min_celebrate = best_luxury;
      }
    } else {
      rates[AI_RATE_SCI] = rate_sci_min;
      rates[AI_RATE_TAX] = rate_tax_min;
      rates[AI_RATE_LUX] = 0;

      rates[AI_RATE_LUX] = MIN(maxrate, rates[AI_RATE_LUX]
                                        + RATE_REMAINS(rates));
      rates[AI_RATE_TAX] = MIN(maxrate, rates[AI_RATE_TAX]
                                        + RATE_REMAINS(rates));
      rates[AI_RATE_SCI] = MIN(maxrate, rates[AI_RATE_SCI]
                                        + RATE_REMAINS(rates));

      pplayer->economic.luxury = rates[AI_RATE_LUX];
      pplayer->economic.tax = rates[AI_RATE_TAX];
      pplayer->economic.science = rates[AI_RATE_SCI];
      dai_evaluate_celebration(ait, pplayer, &cmp, FALSE,
                               &can_celebrate, &total_cities);
      if (can_celebrate * 2 > total_cities) {
        celebrate = AI_CELEBRATION_YES;
        rate_lux_min_celebrate = pplayer->economic.luxury;
      }
    }

    if (celebrate == AI_CELEBRATION_YES) {
      log_base(LOGLEVEL_TAX, "%s [lux] celebration possible (Sci/Lux/Tax)>="
                             "%d/%d/%d (%d of %d cities)",
               player_name(pplayer), rate_sci_min, rate_lux_min_celebrate,
               rate_tax_min, can_celebrate, total_cities);
      if (largepox) {
        log_verbose("%s largepox rapture target: luxury=%d, core-ready=%d/%d",
                    player_name(pplayer), rate_lux_min_celebrate,
                    can_celebrate, total_cities);
      }
    } else {
      log_base(LOGLEVEL_TAX, "%s [lux] no celebration: only %d of %d cities",
               player_name(pplayer), can_celebrate, total_cities);
    }
  }

  if (celebrate != AI_CELEBRATION_YES) {
    /* TODO: Calculate a minimum luxury tax rate.
     *       Add general luxury code here. */
  }

  /* === Set the rates. === */

  /* Reset rates values. */
  rates[AI_RATE_SCI] = 0;
  rates[AI_RATE_TAX] = 0;
  rates[AI_RATE_LUX] = 0;

  /* Now decide that to do ... */
  if (celebrate == AI_CELEBRATION_YES
      && rate_tax_min
         + (largepox ? MAX(rate_sci_min, largepox_science_floor)
                     : rate_sci_min)
         + rate_lux_min_celebrate <= 100) {
    /* Celebration! */
    rates[AI_RATE_SCI]
      = largepox ? MAX(rate_sci_min, largepox_science_floor) : rate_sci_min;
    rates[AI_RATE_TAX] = rate_tax_min;
    rates[AI_RATE_LUX] = rate_lux_min_celebrate;

    log_base(LOGLEVEL_TAX, "%s [res] celebration! (Sci/Lux/Tax)>=%d/%d/%d",
             player_name(pplayer), rates[AI_RATE_SCI], rates[AI_RATE_LUX],
             rates[AI_RATE_TAX]);
  } else {
    /* No celebration */
    celebrate = (celebrate == AI_CELEBRATION_YES) ? AI_CELEBRATION_NO
                                                  : celebrate;

    if (RATE_VALID(rate_tax_balance)) {
      if (RATE_VALID(rate_sci_balance)) {
        if (100 >= rate_tax_balance + rate_sci_balance) {
          /* A balanced treasury and research. */
          rates[AI_RATE_SCI] = rate_sci_balance;
          rates[AI_RATE_TAX] = rate_tax_balance;

          log_base(LOGLEVEL_TAX, "%s [res] balanced! (Sci/Lux/Tax)>=%d/%d/%d",
                   player_name(pplayer), rates[AI_RATE_SCI],
                   rates[AI_RATE_LUX], rates[AI_RATE_TAX]);
        } else {
          /* Try to keep all tech and get as much gold as possible. */
          rates[AI_RATE_SCI] = rate_sci_balance;
          rates[AI_RATE_TAX] = MIN(maxrate, RATE_REMAINS(rates));

          log_base(LOGLEVEL_TAX, "%s [res] balanced sci! tax? "
                                 "(Sci/Lux/Tax)>=%d/%d/%d",
                   player_name(pplayer), rates[AI_RATE_SCI],
                   rates[AI_RATE_LUX], rates[AI_RATE_TAX]);
        }
      } else {
        /* A balanced tax and as much science as possible. */
        rates[AI_RATE_TAX] = rate_tax_balance;
        rates[AI_RATE_SCI] = MIN(maxrate, RATE_REMAINS(rates));

        log_base(LOGLEVEL_TAX, "%s [res] balanced tax! sci? "
                               "(Sci/Lux/Tax)>=%d/%d/%d",
                 player_name(pplayer), rates[AI_RATE_SCI],
                 rates[AI_RATE_LUX], rates[AI_RATE_TAX]);
      }
    } else if (RATE_VALID(rate_sci_balance)) {
      /* Try to keep all techs and get as much gold as possible. */
      rates[AI_RATE_SCI] = rate_sci_balance;
      rates[AI_RATE_TAX] = MIN(maxrate, RATE_REMAINS(rates));

      log_base(LOGLEVEL_TAX, "%s [res] balanced sci! tax? "
                             "(Sci/Lux/Tax)>=%d/%d/%d",
               player_name(pplayer), rates[AI_RATE_SCI],
               rates[AI_RATE_LUX], rates[AI_RATE_TAX]);
    } else {
      /* We need more trade to get a positive gold and science balance. */
      if (!adv_wants_science(pplayer) || dai_on_war_footing(ait, pplayer)) {
        /* Go for gold (improvements and units) and risk the loss of a
         * tech. */
        rates[AI_RATE_TAX] = maxrate;
        rates[AI_RATE_SCI] = MIN(maxrate, RATE_REMAINS(rates));

        log_base(LOGLEVEL_TAX, "%s [res] risk of tech loss! (Sci/Lux/Tax)>="
                               "%d/%d/%d", player_name(pplayer),
                 rates[AI_RATE_SCI], rates[AI_RATE_LUX],
                 rates[AI_RATE_TAX]);
      } else {
        /* Go for science and risk the loss of improvements or units. */
        rates[AI_RATE_SCI] = MAX(maxrate, rate_sci_min);
        rates[AI_RATE_TAX] = MIN(maxrate, RATE_REMAINS(rates));

        log_base(LOGLEVEL_TAX, "%s [res] risk of empty treasury! "
                               "(Sci/Lux/Tax)>=%d/%d/%d",
                 player_name(pplayer), rates[AI_RATE_SCI],
                 rates[AI_RATE_LUX], rates[AI_RATE_TAX]);
      };
    }
  }

  /* Put the remaining to tax or science. */
  if (largepox && largepox_science_floor == 0) {
    rates[AI_RATE_TAX] = MIN(maxrate, rates[AI_RATE_TAX]
                                      + RATE_REMAINS(rates));
    rates[AI_RATE_SCI] = MIN(maxrate, rates[AI_RATE_SCI]
                                      + RATE_REMAINS(rates));
    rates[AI_RATE_LUX] = MIN(maxrate, rates[AI_RATE_LUX]
                                      + RATE_REMAINS(rates));
  } else if (dai_largepox_growth_focus(ait, pplayer)) {
    rates[AI_RATE_TAX] = MIN(maxrate, rates[AI_RATE_TAX]
                                      + RATE_REMAINS(rates));
    rates[AI_RATE_SCI] = MIN(maxrate, rates[AI_RATE_SCI]
                                      + RATE_REMAINS(rates));
    rates[AI_RATE_LUX] = MIN(maxrate, rates[AI_RATE_LUX]
                                      + RATE_REMAINS(rates));
  } else if (!adv_wants_science(pplayer)) {
    rates[AI_RATE_TAX] = MIN(maxrate, rates[AI_RATE_TAX]
                                      + RATE_REMAINS(rates));
    rates[AI_RATE_LUX] = MIN(maxrate, rates[AI_RATE_LUX]
                                      + RATE_REMAINS(rates));
    rates[AI_RATE_SCI] = MIN(maxrate, rates[AI_RATE_SCI]
                                      + RATE_REMAINS(rates));
  } else if (dai_on_war_footing(ait, pplayer)) {
    rates[AI_RATE_TAX] = MIN(maxrate, rates[AI_RATE_TAX]
                                      + RATE_REMAINS(rates));
    rates[AI_RATE_SCI] = MIN(maxrate, rates[AI_RATE_SCI]
                                      + RATE_REMAINS(rates));
    rates[AI_RATE_LUX] = MIN(maxrate, rates[AI_RATE_LUX]
                                      + RATE_REMAINS(rates));
  } else {
    rates[AI_RATE_SCI] = MIN(maxrate, rates[AI_RATE_SCI]
                                      + RATE_REMAINS(rates));
    rates[AI_RATE_TAX] = MIN(maxrate, rates[AI_RATE_TAX]
                                      + RATE_REMAINS(rates));
    rates[AI_RATE_LUX] = MIN(maxrate, rates[AI_RATE_LUX]
                                      + RATE_REMAINS(rates));
  }

  /* A failed celebration query must not silently put largepox back at zero
   * science. Preserve the chosen luxury rate where possible and finance the
   * strategic research floor from tax first. */
  if (largepox && rates[AI_RATE_SCI] < largepox_science_floor) {
    int missing = largepox_science_floor - rates[AI_RATE_SCI];
    int shifted = MIN(missing, rates[AI_RATE_TAX]);

    rates[AI_RATE_TAX] -= shifted;
    rates[AI_RATE_SCI] += shifted;
    missing -= shifted;

    shifted = MIN(missing, rates[AI_RATE_LUX]);
    rates[AI_RATE_LUX] -= shifted;
    rates[AI_RATE_SCI] += shifted;
  }

  if (largepox) {
    const struct advance *trade_advance = advance_by_rule_name("Trade");

    if (trade_advance != nullptr
        && research_invention_state(research, advance_number(trade_advance))
           != TECH_KNOWN) {
      /* The measured human opening had its first Caravan moving on turn 39,
       * while the generic tax solver left the AI one strategic technology
       * and roughly six caravan turns behind.  Before Trade exists there is
       * nothing worth celebrating and very little to rush-buy, so use the
       * full legal science rate and keep the remainder as seed gold. */
      rates[AI_RATE_SCI] = maxrate;
      rates[AI_RATE_LUX] = 0;
      rates[AI_RATE_TAX] = 100 - maxrate;
    }
  }

  /* Check and set the calculated rates. */
  fc_assert_ret(0 <= rates[AI_RATE_SCI] && rates[AI_RATE_SCI] <= maxrate);
  fc_assert_ret(0 <= rates[AI_RATE_TAX] && rates[AI_RATE_TAX] <= maxrate);
  fc_assert_ret(0 <= rates[AI_RATE_LUX] && rates[AI_RATE_LUX] <= maxrate);
  fc_assert_ret(rates[AI_RATE_SCI] + rates[AI_RATE_TAX] + rates[AI_RATE_LUX]
                == 100);

  log_base(LOGLEVEL_TAX, "%s [new] (Sci/Lux/Tax)=%d/%d/%d",
           player_name(pplayer), rates[AI_RATE_SCI], rates[AI_RATE_LUX],
           rates[AI_RATE_TAX]);

  pplayer->economic.science = rates[AI_RATE_SCI];
  pplayer->economic.tax = rates[AI_RATE_TAX];
  pplayer->economic.luxury = rates[AI_RATE_LUX];

  /* === Cleanup === */

  /* Cancel all celebrations from the last turn. */
  adv->celebrate = FALSE;

  /* Now do celebrate or reset the city states if needed. */
  if (celebrate == AI_CELEBRATION_YES) {
    log_base(LOGLEVEL_TAX, "*** %s CELEBRATES! ***", player_name(pplayer));

    /* We do celebrate! */
    adv->celebrate = TRUE;

    if (largepox) {
      int city_count = city_list_size(pplayer->cities);
      int selected = 0;
      struct city *ordered[city_count + 1];

      city_list_iterate(pplayer->cities, pcity) {
        /* Reconsider the complete eligible core after releasing its shared
         * tiles.  A city whose independent query failed only because a
         * neighbour held its food must not be omitted from the global pass. */
        if (!dai_largepox_city_is_support(ait, pplayer, pcity)
            && city_size_get(pcity) >= 2) {
          ordered[selected++] = pcity;
        }
      } city_list_iterate_end;

      /* Give scarce shared food tiles to the least flexible cities first.
       * Large cities normally need more worked tiles; lower current food is
       * the tie-breaker. Flexible smaller neighbours are then re-solved
       * against what remains instead of permanently starving late cities in
       * list order. */
      for (int i = 0; i < selected; i++) {
        for (int j = i + 1; j < selected; j++) {
          if (city_size_get(ordered[j]) > city_size_get(ordered[i])
              || (city_size_get(ordered[j]) == city_size_get(ordered[i])
                  && ordered[j]->surplus[O_FOOD]
                     < ordered[i]->surplus[O_FOOD])) {
            struct city *swap = ordered[i];

            ordered[i] = ordered[j];
            ordered[j] = swap;
          }
        }
      }

      dai_release_celebration_workers(nmap, ordered, selected);

      for (int i = 0; i < selected; i++) {
        dai_apply_celebration_city(ait, nmap, ordered[i], &cmp);
      }

    } else {
      city_list_iterate(pplayer->cities, pcity) {
        if (def_ai_city_data(pcity, ait)->celebrate) {
          dai_apply_celebration_city(ait, nmap, pcity, &cmp);
        }
      } city_list_iterate_end;
    }
  } else if (celebrate == AI_CELEBRATION_NO) {
    city_list_iterate(pplayer->cities, pcity) {
      /* KLUDGE: Must refresh to restore the original values which
       * were clobbered in cm_query_result(), after the tax rates
       * were changed. */
      city_refresh_from_main_map(nmap, pcity, nullptr);
    } city_list_iterate_end;
  }

  send_player_info_c(pplayer, pplayer->connections);

#ifdef DEBUG_TIMERS
  timer_stop(taxtimer);
  log_base(LOGLEVEL_TAX, "Tax calculation for %s (player %d) in %.3f "
                         "seconds.", player_name(pplayer),
           player_index(pplayer), timer_read_seconds(taxtimer));
  timer_destroy(taxtimer);
#endif /* DEBUG_TIMERS */
}
#undef RATE_NOT_SET
#undef RATE_VALID
#undef RATE_REMAINS

/*************************************************************************//**
  Change the government form, if it can and there is a good reason.
*****************************************************************************/
static void dai_manage_government(struct ai_type *ait, struct player *pplayer)
{
  struct adv_data *adv = adv_data_get(pplayer, nullptr);

  if (!pplayer->is_alive || has_handicap(pplayer, H_AWAY)) {
    return;
  }

  if (dai_largepox_enabled()) {
    struct government *republic = government_by_rule_name("Republic");
    struct government *democracy = government_by_rule_name("Democracy");
    struct government *target = nullptr;

    /* The largepox research milestones deliberately unlock the first
     * rapture government and then its mature replacement. Do not let the
     * generic short-term government valuation switch this strategy back to
     * a non-celebrating government. */
    if (democracy != nullptr
        && can_change_to_government(pplayer, democracy)) {
      target = democracy;
    } else if (republic != nullptr
               && can_change_to_government(pplayer, republic)) {
      target = republic;
    }

    if (target != nullptr && target != government_of_player(pplayer)) {
      dai_government_change(pplayer, target);
    }
    return;
  }

  if (adv->goal.revolution != government_of_player(pplayer)) {
    dai_government_change(pplayer, adv->goal.revolution); /* Change */
  }

  /* Crank up tech want */
  if (adv->goal.govt.req == A_UNSET
      || research_invention_state(research_get(pplayer),
                                  adv->goal.govt.req) == TECH_KNOWN) {
    return; /* Already got it! */
  } else if (adv->goal.govt.val > 0) {
    /* We have few cities in the beginning, compensate for this to ensure
     * that we are sufficiently forward-looking. */
    int want = MAX(adv->goal.govt.val, 100);
    struct nation_type *pnation = nation_of_player(pplayer);
    struct ai_plr *plr_data = def_ai_player_data(pplayer, ait);

    if (government_of_player(pplayer) == init_government_of_nation(pnation)) {
      /* Default government is the crappy one we start in (like Despotism).
       * We want something better pretty soon! */
      want += 25 * game.info.turn;
    }
    plr_data->tech_want[adv->goal.govt.req] += want;
    TECH_LOG(ait, LOG_DEBUG, pplayer, advance_by_number(adv->goal.govt.req),
             "dai_manage_government() + %d for %s",
             want,
             government_rule_name(adv->goal.govt.gov));
  }
}

/*************************************************************************//**
  Activities to be done by AI _before_ human turn. Here we just move the
  units intelligently.
*****************************************************************************/
void dai_do_first_activities(struct ai_type *ait, struct player *pplayer)
{
  TIMING_LOG(AIT_ALL, TIMER_START);
  dai_assess_danger_player(ait, &(wld.map), pplayer);
  /* TODO: Make assess_danger save information on what is threatening
   * us and make dai_manage_units and Co act upon this information, trying
   * to eliminate the source of danger */

  TIMING_LOG(AIT_UNITS, TIMER_START);
  dai_manage_units(ait, pplayer);
  TIMING_LOG(AIT_UNITS, TIMER_STOP);
  /* STOP.  Everything else is at end of turn. */

  TIMING_LOG(AIT_ALL, TIMER_STOP);

  flush_packets(); /* AIs can be such spammers... */
}

/*************************************************************************//**
  Activities to be done by AI _after_ human turn. Here we respond to
  dangers created by human and AI opposition by ordering defenders in
  cities and setting taxes accordingly. We also do other duties.

  We do _not_ move units here, otherwise humans complain that AI moves
  twice.
*****************************************************************************/
void dai_do_last_activities(struct ai_type *ait, struct player *pplayer)
{
  TIMING_LOG(AIT_ALL, TIMER_START);
  dai_clear_tech_wants(ait, pplayer);

  dai_manage_government(ait, pplayer);
  dai_adjust_policies(ait, pplayer);
  dai_largepox_update(ait, pplayer);
  TIMING_LOG(AIT_TAXES, TIMER_START);
  dai_manage_taxes(ait, pplayer);
  TIMING_LOG(AIT_TAXES, TIMER_STOP);
  dai_largepox_update(ait, pplayer);
  TIMING_LOG(AIT_CITIES, TIMER_START);
  dai_manage_cities(ait, pplayer);
  TIMING_LOG(AIT_CITIES, TIMER_STOP);
  TIMING_LOG(AIT_TECH, TIMER_START);
  dai_manage_tech(ait, pplayer);
  TIMING_LOG(AIT_TECH, TIMER_STOP);
  dai_manage_spaceship(pplayer);

  TIMING_LOG(AIT_ALL, TIMER_STOP);
}
