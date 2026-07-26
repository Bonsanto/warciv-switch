/***********************************************************************
 Freeciv - Copyright (C) 1996 - Freeciv Development Team
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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* utility */
#include "fcintl.h"
#include "mem.h"
#include "support.h"

/* common */
#include "actions.h"
#include "city.h"
#include "game.h"
#include "map.h"
#include "traderoutes.h"
#include "unit.h"

/* client */
#include "chatline_common.h"
#include "client_main.h"
#include "control.h"
#include "goto.h"
#include "mapview_common.h"

#include "tradeplanning.h"

struct trade_planning_city {
  struct city *city;
  struct tile *tile;
  bool is_virtual;
};

#define SPECLIST_TAG trade_planning_city
#define SPECLIST_TYPE struct trade_planning_city
#include "speclist.h"

#define trade_planning_city_list_iterate(city_list, pcity)                 \
  TYPED_LIST_ITERATE(struct trade_planning_city, city_list, pcity)
#define trade_planning_city_list_iterate_end LIST_ITERATE_END

struct trade_candidate {
  int first;
  int second;
  int value;
};

static struct trade_planning_city_list *planning_cities;
static struct trade_planning_route_list *planning_routes;
static bool planning_selecting;
static bool planning_automatic;

/* Caravans traveling to change their home city (the "auto caravan" key);
 * continued from trade_planning_pulse() once the new home is confirmed. */
#define TRADE_PLANNING_REHOME_MAX 128
static struct trade_planning_rehome {
  int unit_id;
  int city_id;
} rehome_pending[TRADE_PLANNING_REHOME_MAX];
static int rehome_count = 0;

/**********************************************************************//**
  Initialize the client-side trade planner.
**************************************************************************/
void trade_planning_init(void)
{
  if (planning_cities == NULL) {
    planning_cities = trade_planning_city_list_new();
  }
  if (planning_routes == NULL) {
    planning_routes = trade_planning_route_list_new();
  }

  planning_selecting = FALSE;
  planning_automatic = FALSE;
  rehome_count = 0;
}

/**********************************************************************//**
  Ensure that the planner's containers exist.
**************************************************************************/
static void trade_planning_ensure_init(void)
{
  if (planning_cities == NULL || planning_routes == NULL) {
    trade_planning_init();
  }
}

/**********************************************************************//**
  Destroy every calculated route, preserving selected locations.
**************************************************************************/
static void trade_planning_clear_routes(void)
{
  if (planning_routes == NULL) {
    return;
  }

  while (trade_planning_route_list_size(planning_routes) > 0) {
    struct trade_planning_route *proute =
      trade_planning_route_list_get(planning_routes, 0);

    trade_planning_route_list_remove(planning_routes, proute);
    free(proute);
  }
}

/**********************************************************************//**
  Clear all selected locations and calculated routes.
**************************************************************************/
void trade_planning_clear(void)
{
  trade_planning_ensure_init();
  trade_planning_clear_routes();

  while (trade_planning_city_list_size(planning_cities) > 0) {
    struct trade_planning_city *pcity =
      trade_planning_city_list_get(planning_cities, 0);

    trade_planning_city_list_remove(planning_cities, pcity);
    if (pcity->is_virtual) {
      destroy_city_virtual(pcity->city);
    }
    free(pcity);
  }

  update_map_canvas_visible();
}

/**********************************************************************//**
  Release all planner state before the client game is freed or reset.
**************************************************************************/
void trade_planning_free(void)
{
  if (planning_cities == NULL && planning_routes == NULL) {
    return;
  }

  trade_planning_clear();
  trade_planning_city_list_destroy(planning_cities);
  trade_planning_route_list_destroy(planning_routes);
  planning_cities = NULL;
  planning_routes = NULL;
  planning_selecting = FALSE;
  planning_automatic = FALSE;
}

/**********************************************************************//**
  Add a city to the collection unless its tile is already present.
**************************************************************************/
static bool trade_planning_add_city(struct city *pcity, bool is_virtual)
{
  struct trade_planning_city *item;

  if (pcity == NULL) {
    return FALSE;
  }

  trade_planning_city_list_iterate(planning_cities, existing) {
    if (existing->tile == city_tile(pcity)) {
      return FALSE;
    }
  } trade_planning_city_list_iterate_end;

  item = fc_malloc(sizeof(*item));
  item->city = pcity;
  item->tile = city_tile(pcity);
  item->is_virtual = is_virtual;
  trade_planning_city_list_append(planning_cities, item);
  trade_planning_clear_routes();

  return TRUE;
}

/**********************************************************************//**
  Replace the location collection with all cities owned by the player.
**************************************************************************/
void trade_planning_add_all_cities(void)
{
  struct player *pplayer = client_player();
  char buf[256];

  trade_planning_clear();
  if (pplayer == NULL) {
    return;
  }

  city_list_iterate(pplayer->cities, pcity) {
    trade_planning_add_city(pcity, FALSE);
  } city_list_iterate_end;

  fc_snprintf(buf, sizeof(buf),
              PL_("Added %d city to trade planning.",
                  "Added %d cities to trade planning.",
                  trade_planning_city_list_size(planning_cities)),
              trade_planning_city_list_size(planning_cities));
  output_window_append(ftc_client, buf);
  update_map_canvas_visible();
}

/**********************************************************************//**
  Add or remove a real or virtual city at the given map location.
**************************************************************************/
bool trade_planning_toggle_tile(struct tile *ptile)
{
  struct city *pcity;
  char buf[256];

  trade_planning_ensure_init();
  if (ptile == NULL) {
    return FALSE;
  }

  for (int i = trade_planning_city_list_size(planning_cities) - 1;
       i >= 0; i--) {
    struct trade_planning_city *item =
      trade_planning_city_list_get(planning_cities, i);

    if (item->tile == ptile) {
      fc_snprintf(buf, sizeof(buf),
                  _("Removed %s from trade planning."),
                  city_name_get(item->city));
      output_window_append(ftc_client, buf);
      trade_planning_city_list_remove(planning_cities, item);
      if (item->is_virtual) {
        destroy_city_virtual(item->city);
      }
      free(item);
      trade_planning_clear_routes();
      update_map_canvas_visible();
      return TRUE;
    }
  }

  pcity = tile_city(ptile);
  if (pcity == NULL) {
    if (client_player() == NULL) {
      return FALSE;
    }
    pcity = create_city_virtual(client_player(), ptile, _("Virtual"));
    if (!trade_planning_add_city(pcity, TRUE)) {
      destroy_city_virtual(pcity);
      return FALSE;
    }
  } else if (!trade_planning_add_city(pcity, FALSE)) {
    return FALSE;
  }

  fc_snprintf(buf, sizeof(buf), _("Added %s to trade planning."),
              city_name_get(pcity));
  output_window_append(ftc_client, buf);
  update_map_canvas_visible();
  return TRUE;
}

/**********************************************************************//**
  Return whether the tile is part of the current location collection.
**************************************************************************/
bool trade_planning_tile_is_selected(const struct tile *ptile)
{
  trade_planning_ensure_init();

  trade_planning_city_list_iterate(planning_cities, item) {
    if (item->tile == ptile) {
      return TRUE;
    }
  } trade_planning_city_list_iterate_end;

  return FALSE;
}

/**********************************************************************//**
  Return a deterministic pseudo-random value without modifying Freeciv's
  global random state.
**************************************************************************/
static uint32_t trade_planning_random(uint32_t *state)
{
  uint32_t value = *state;

  if (value == 0) {
    value = 0x6d2b79f5U;
  }
  value ^= value << 13;
  value ^= value >> 17;
  value ^= value << 5;
  *state = value;

  return value;
}

/**********************************************************************//**
  Sort candidate indices by descending recurring trade value.
**************************************************************************/
static void trade_planning_sort_by_value(int *indices, int count,
                                         const struct trade_candidate *candidates)
{
  for (int i = 1; i < count; i++) {
    int value = indices[i];
    int j = i - 1;

    while (j >= 0
           && candidates[indices[j]].value < candidates[value].value) {
      indices[j + 1] = indices[j];
      j--;
    }
    indices[j + 1] = value;
  }
}

/**********************************************************************//**
  Shuffle candidate indices with a deterministic planner-local generator.
**************************************************************************/
static void trade_planning_shuffle(int *indices, int count, uint32_t *state)
{
  for (int i = count - 1; i > 0; i--) {
    int other = trade_planning_random(state) % (i + 1);
    int value = indices[i];

    indices[i] = indices[other];
    indices[other] = value;
  }
}

/**********************************************************************//**
  Refresh real-city pointers and discard locations whose city disappeared.
**************************************************************************/
static void trade_planning_refresh_cities(void)
{
  for (int i = trade_planning_city_list_size(planning_cities) - 1;
       i >= 0; i--) {
    struct trade_planning_city *item =
      trade_planning_city_list_get(planning_cities, i);

    if (item->is_virtual) {
      struct city *current = tile_city(item->tile);

      if (current != NULL) {
        destroy_city_virtual(item->city);
        item->city = current;
        item->is_virtual = FALSE;
      }
    } else {
      struct city *current = tile_city(item->tile);

      if (current == NULL) {
        trade_planning_city_list_remove(planning_cities, item);
        free(item);
      } else {
        item->city = current;
      }
    }
  }
}

/**********************************************************************//**
  Calculate a high-value set of legal routes for the selected locations.
**************************************************************************/
void trade_planning_calculate(void)
{
  struct trade_candidate *candidates;
  int *capacity;
  int *order;
  int *best;
  int *used;
  int *selected;
  int city_count;
  int candidate_count = 0;
  int candidate_max;
  int best_count = 0;
  int best_value = -1;
  int trials;
  uint32_t random_state = 0x9e3779b9U;
  char buf[256];

  trade_planning_ensure_init();
  trade_planning_clear_routes();
  trade_planning_refresh_cities();
  city_count = trade_planning_city_list_size(planning_cities);
  candidate_max = city_count * MAX(0, city_count - 1) / 2;

  candidates = fc_malloc(MAX(1, candidate_max) * sizeof(*candidates));
  capacity = fc_calloc(MAX(1, city_count), sizeof(*capacity));

  for (int i = 0; i < city_count; i++) {
    struct trade_planning_city *item =
      trade_planning_city_list_get(planning_cities, i);

    capacity[i] =
      MAX(0, (int) max_trade_routes(item->city)
             - city_num_trade_routes(item->city));
    random_state ^= (uint32_t) tile_index(item->tile)
                    + 0x9e3779b9U
                    + (random_state << 6) + (random_state >> 2);
  }

  for (int first = 0; first < city_count; first++) {
    struct trade_planning_city *city1 =
      trade_planning_city_list_get(planning_cities, first);

    if (capacity[first] == 0) {
      continue;
    }
    for (int second = first + 1; second < city_count; second++) {
      struct trade_planning_city *city2 =
        trade_planning_city_list_get(planning_cities, second);

      if (capacity[second] == 0
          || have_cities_trade_route(city1->city, city2->city)
          || !can_establish_trade_route(city1->city, city2->city,
                                        GOODS_HIGH_PRIORITY)) {
        continue;
      }

      candidates[candidate_count].first = first;
      candidates[candidate_count].second = second;
      candidates[candidate_count].value =
        trade_base_between_cities(city1->city, city2->city);
      candidate_count++;
    }
  }

  order = fc_malloc(MAX(1, candidate_count) * sizeof(*order));
  best = fc_malloc(MAX(1, candidate_count) * sizeof(*best));
  used = fc_malloc(MAX(1, city_count) * sizeof(*used));
  selected = fc_malloc(MAX(1, candidate_count) * sizeof(*selected));

  for (int i = 0; i < candidate_count; i++) {
    order[i] = i;
  }

  trials = MIN(512, MAX(64, candidate_count * 2));
  for (int trial = 0; trial < trials; trial++) {
    int selected_count = 0;
    int value = 0;

    memset(used, 0, MAX(1, city_count) * sizeof(*used));
    if (trial == 0) {
      trade_planning_sort_by_value(order, candidate_count, candidates);
    } else {
      trade_planning_shuffle(order, candidate_count, &random_state);
    }

    for (int i = 0; i < candidate_count; i++) {
      const struct trade_candidate *candidate = &candidates[order[i]];

      if (used[candidate->first] >= capacity[candidate->first]
          || used[candidate->second] >= capacity[candidate->second]) {
        continue;
      }

      used[candidate->first]++;
      used[candidate->second]++;
      selected[selected_count++] = order[i];
      value += candidate->value;
    }

    if (value > best_value
        || (value == best_value && selected_count > best_count)) {
      memcpy(best, selected, selected_count * sizeof(*best));
      best_count = selected_count;
      best_value = value;
    }
  }

  trade_planning_sort_by_value(best, best_count, candidates);
  for (int i = 0; i < best_count; i++) {
    const struct trade_candidate *candidate = &candidates[best[i]];
    struct trade_planning_city *city1 =
      trade_planning_city_list_get(planning_cities, candidate->first);
    struct trade_planning_city *city2 =
      trade_planning_city_list_get(planning_cities, candidate->second);
    struct trade_planning_route *route = fc_malloc(sizeof(*route));

    route->tile1 = city1->tile;
    route->tile2 = city2->tile;
    route->caravan_id = IDENTITY_NUMBER_ZERO;
    route->value = candidate->value;
    trade_planning_route_list_append(planning_routes, route);
  }

  fc_snprintf(buf, sizeof(buf),
              PL_("Planned %d trade route with a recurring value of %d.",
                  "Planned %d trade routes with a recurring value of %d.",
                  best_count),
              best_count, MAX(0, best_value));
  output_window_append(ftc_client, buf);

  free(selected);
  free(used);
  free(best);
  free(order);
  free(capacity);
  free(candidates);

  if (planning_automatic) {
    trade_planning_dispatch_all();
  }
  update_map_canvas_visible();
}

/**********************************************************************//**
  Return whether map clicks currently select trade-planning locations.
**************************************************************************/
bool trade_planning_is_selecting(void)
{
  return planning_selecting;
}

/**********************************************************************//**
  Enable or disable map location selection.
**************************************************************************/
void trade_planning_set_selecting(bool selecting)
{
  trade_planning_ensure_init();
  planning_selecting = selecting;
  output_window_append(ftc_client,
                       selecting
                       ? _("Trade location selection enabled; click cities "
                           "or prospective city sites on the map.")
                       : _("Trade location selection disabled."));
}

/**********************************************************************//**
  Return whether idle caravans are dispatched automatically.
**************************************************************************/
bool trade_planning_is_automatic(void)
{
  return planning_automatic;
}

/**********************************************************************//**
  Send one idle caravan along an unassigned route from its home city.
**************************************************************************/
static bool trade_planning_dispatch_caravan(struct unit *punit)
{
  struct city *homecity;

  if (punit == NULL
      || unit_owner(punit) != client_player()
      || unit_has_orders(punit)
      || punit->activity != ACTIVITY_IDLE
      || !unit_can_do_action(punit, ACTION_TRADE_ROUTE)) {
    return FALSE;
  }

  homecity = game_city_by_number(punit->homecity);
  if (homecity == NULL) {
    return FALSE;
  }

  trade_planning_route_list_iterate(planning_routes, route) {
    if (route->caravan_id == punit->id) {
      return FALSE;
    }
  } trade_planning_route_list_iterate_end;

  trade_planning_route_list_iterate(planning_routes, route) {
    struct tile *destination;

    if (route->caravan_id != IDENTITY_NUMBER_ZERO
        || (route->tile1 != city_tile(homecity)
            && route->tile2 != city_tile(homecity))) {
      continue;
    }

    destination =
      route->tile1 == city_tile(homecity) ? route->tile2 : route->tile1;
    if (tile_city(destination) != NULL
        && send_goto_tile_and_action(punit, destination,
                                     ACTION_TRADE_ROUTE)) {
      route->caravan_id = punit->id;
      return TRUE;
    }
  } trade_planning_route_list_iterate_end;

  return FALSE;
}

/**********************************************************************//**
  Dispatch all eligible idle caravans and retry interrupted assignments.
**************************************************************************/
int trade_planning_dispatch_all(void)
{
  struct player *pplayer = client_player();
  int dispatched = 0;
  bool changed = FALSE;

  trade_planning_ensure_init();
  if (pplayer == NULL || !can_client_issue_orders()) {
    return 0;
  }

  for (int i = trade_planning_route_list_size(planning_routes) - 1;
       i >= 0; i--) {
    struct trade_planning_route *route =
      trade_planning_route_list_get(planning_routes, i);
    struct city *city1 = tile_city(route->tile1);
    struct city *city2 = tile_city(route->tile2);

    if (city1 != NULL && city2 != NULL
        && have_cities_trade_route(city1, city2)) {
      trade_planning_route_list_remove(planning_routes, route);
      free(route);
      changed = TRUE;
    } else if (route->caravan_id != IDENTITY_NUMBER_ZERO) {
      struct unit *assigned = game_unit_by_number(route->caravan_id);

      if (assigned == NULL || !unit_has_orders(assigned)) {
        route->caravan_id = IDENTITY_NUMBER_ZERO;
        changed = TRUE;
      }
    }
  }

  unit_list_iterate(pplayer->units, punit) {
    if (trade_planning_dispatch_caravan(punit)) {
      dispatched++;
      changed = TRUE;
    }
  } unit_list_iterate_end;

  if (changed) {
    update_map_canvas_visible();
  }
  return dispatched;
}

/**********************************************************************//**
  Enable or disable automatic caravan dispatch.
**************************************************************************/
int trade_planning_set_automatic(bool automatic)
{
  char buf[256];
  int dispatched = 0;

  trade_planning_ensure_init();
  planning_automatic = automatic;
  if (automatic) {
    dispatched = trade_planning_dispatch_all();
    fc_snprintf(buf, sizeof(buf),
                PL_("Automatic caravan movement enabled; %d unit dispatched.",
                    "Automatic caravan movement enabled; %d units dispatched.",
                    dispatched),
                dispatched);
    output_window_append(ftc_client, buf);
  } else {
    output_window_append(ftc_client,
                         _("Automatic caravan movement disabled."));
  }

  return dispatched;
}

/**********************************************************************//**
  Forget any pending rehome bookkeeping for the unit.
**************************************************************************/
static void trade_planning_rehome_forget(int unit_id)
{
  for (int i = 0; i < rehome_count; i++) {
    if (rehome_pending[i].unit_id == unit_id) {
      rehome_pending[i] = rehome_pending[rehome_count - 1];
      rehome_count--;
      return;
    }
  }
}

/**********************************************************************//**
  Remember that the unit is traveling to change its home city.
**************************************************************************/
static void trade_planning_rehome_remember(int unit_id, int city_id)
{
  trade_planning_rehome_forget(unit_id);
  if (rehome_count < TRADE_PLANNING_REHOME_MAX) {
    rehome_pending[rehome_count].unit_id = unit_id;
    rehome_pending[rehome_count].city_id = city_id;
    rehome_count++;
  }
}

/**********************************************************************//**
  Pick the closest own city that still has an unassigned planned route
  with an existing city on the other end.
**************************************************************************/
static struct city *trade_planning_best_rehome_city(const struct unit *punit)
{
  struct city *best = NULL;
  int best_dist = 0;

  trade_planning_route_list_iterate(planning_routes, route) {
    struct tile *ends[2] = { route->tile1, route->tile2 };

    if (route->caravan_id != IDENTITY_NUMBER_ZERO) {
      continue;
    }
    for (int i = 0; i < 2; i++) {
      struct city *pcity = tile_city(ends[i]);
      int dist;

      if (pcity == NULL || city_owner(pcity) != client_player()
          || tile_city(ends[1 - i]) == NULL) {
        continue;
      }
      dist = real_map_distance(unit_tile(punit), ends[i]);
      if (best == NULL || dist < best_dist) {
        best = pcity;
        best_dist = dist;
      }
    }
  } trade_planning_route_list_iterate_end;

  return best;
}

/**********************************************************************//**
  "Auto caravan": send the unit along an unassigned planned route from its
  home city; when its home has none (a support city, or a city whose
  planned routes are all done), send it to rehome at the closest planned
  city with work left and continue from trade_planning_pulse().
**************************************************************************/
bool trade_planning_auto_caravan(struct unit *punit)
{
  struct city *homecity;
  struct city *rehome;
  char buf[256];

  trade_planning_ensure_init();
  if (punit == NULL || unit_owner(punit) != client_player()
      || !can_client_issue_orders()
      || !unit_can_do_action(punit, ACTION_TRADE_ROUTE)) {
    return FALSE;
  }

  /* An explicit request overrides a stale assignment. */
  trade_planning_route_list_iterate(planning_routes, route) {
    if (route->caravan_id == punit->id) {
      route->caravan_id = IDENTITY_NUMBER_ZERO;
    }
  } trade_planning_route_list_iterate_end;
  trade_planning_rehome_forget(punit->id);

  /* First choice: an unassigned planned route from the home city. */
  homecity = game_city_by_number(punit->homecity);
  if (homecity != NULL) {
    trade_planning_route_list_iterate(planning_routes, route) {
      struct tile *destination;

      if (route->caravan_id != IDENTITY_NUMBER_ZERO
          || (route->tile1 != city_tile(homecity)
              && route->tile2 != city_tile(homecity))) {
        continue;
      }
      destination = route->tile1 == city_tile(homecity)
        ? route->tile2 : route->tile1;
      if (tile_city(destination) != NULL
          && send_goto_tile_and_action(punit, destination,
                                       ACTION_TRADE_ROUTE)) {
        route->caravan_id = punit->id;
        update_map_canvas_visible();
        return TRUE;
      }
    } trade_planning_route_list_iterate_end;
  }

  /* Support caravan: rehome to the closest planned city with work left. */
  rehome = trade_planning_best_rehome_city(punit);
  if (rehome == NULL) {
    output_window_append(ftc_client,
                         _("No unassigned trade routes left in the plan."));
    return FALSE;
  }
  if (!unit_can_do_action(punit, ACTION_HOME_CITY)
      || !send_goto_tile_and_action(punit, city_tile(rehome),
                                    ACTION_HOME_CITY)) {
    return FALSE;
  }
  trade_planning_rehome_remember(punit->id, rehome->id);
  fc_snprintf(buf, sizeof(buf),
              _("Caravan heading to %s to join the trade plan."),
              city_name_get(rehome));
  output_window_append(ftc_client, buf);
  update_map_canvas_visible();

  return TRUE;
}

/**********************************************************************//**
  Periodic driver: continue caravans that finished rehoming, then run the
  automatic dispatcher when it is enabled. Called at least once per turn.
**************************************************************************/
void trade_planning_pulse(void)
{
  trade_planning_ensure_init();
  if (!can_client_issue_orders()) {
    return;
  }

  for (int i = rehome_count - 1; i >= 0; i--) {
    struct unit *punit = game_unit_by_number(rehome_pending[i].unit_id);
    int city_id = rehome_pending[i].city_id;

    if (punit == NULL || unit_owner(punit) != client_player()) {
      rehome_pending[i] = rehome_pending[rehome_count - 1];
      rehome_count--;
      continue;
    }
    if (unit_has_orders(punit)) {
      continue;                 /* Still traveling. */
    }
    rehome_pending[i] = rehome_pending[rehome_count - 1];
    rehome_count--;
    if (punit->homecity == city_id) {
      /* Rehome complete: pick up a route from the new home. */
      trade_planning_auto_caravan(punit);
    }
    /* Otherwise the trip was interrupted; leave the unit to the user. */
  }

  if (planning_automatic) {
    trade_planning_dispatch_all();
  }
}

/**********************************************************************//**
  Establish an assigned trade route when its caravan reaches the intended
  destination. The GUI remains responsible for advancing its action queue.
**************************************************************************/
bool trade_planning_handle_arrival(struct unit *actor_unit,
                                   struct city *target_city,
                                   const struct act_prob *act_probs)
{
  struct city *homecity;

  if (planning_routes == NULL || actor_unit == NULL || target_city == NULL) {
    return FALSE;
  }

  homecity = game_city_by_number(actor_unit->homecity);
  for (int i = 0; i < trade_planning_route_list_size(planning_routes); i++) {
    struct trade_planning_route *route =
      trade_planning_route_list_get(planning_routes, i);
    struct tile *destination = NULL;

    if (route->caravan_id != actor_unit->id) {
      continue;
    }
    if (homecity != NULL) {
      destination =
        route->tile1 == city_tile(homecity) ? route->tile2 : route->tile1;
    }

    if (destination == city_tile(target_city)
        && game_unit_by_number(actor_unit->id) != NULL
        && game_city_by_number(target_city->id) != NULL
        && action_prob_possible(act_probs[ACTION_TRADE_ROUTE])) {
      request_do_action(ACTION_TRADE_ROUTE, actor_unit->id,
                        target_city->id, 0, "");
      trade_planning_route_list_remove(planning_routes, route);
      free(route);
      update_map_canvas_visible();
      return TRUE;
    }

    if (destination == city_tile(target_city)) {
      route->caravan_id = IDENTITY_NUMBER_ZERO;
      update_map_canvas_visible();
    }
  }

  return FALSE;
}

/**********************************************************************//**
  Return the calculated route collection for map drawing.
**************************************************************************/
const struct trade_planning_route_list *trade_planning_routes(void)
{
  trade_planning_ensure_init();
  return planning_routes;
}

/**********************************************************************//**
  Return the number of selected real or prospective city locations.
**************************************************************************/
int trade_planning_location_count(void)
{
  trade_planning_ensure_init();
  return trade_planning_city_list_size(planning_cities);
}
