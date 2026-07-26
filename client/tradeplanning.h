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
#ifndef FC__TRADEPLANNING_H
#define FC__TRADEPLANNING_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* utility */
#include "support.h"            /* bool */

struct act_prob;
struct city;
struct tile;
struct unit;

struct trade_planning_route {
  struct tile *tile1;
  struct tile *tile2;
  int caravan_id;
  int value;
};

#define SPECLIST_TAG trade_planning_route
#define SPECLIST_TYPE struct trade_planning_route
#include "speclist.h"

#define trade_planning_route_list_iterate(route_list, proute)             \
  TYPED_LIST_ITERATE(struct trade_planning_route, route_list, proute)
#define trade_planning_route_list_iterate_end LIST_ITERATE_END

void trade_planning_init(void);
void trade_planning_free(void);

void trade_planning_add_all_cities(void);
bool trade_planning_toggle_tile(struct tile *ptile);
bool trade_planning_tile_is_selected(const struct tile *ptile);
void trade_planning_calculate(void);
void trade_planning_clear(void);

bool trade_planning_is_selecting(void);
void trade_planning_set_selecting(bool selecting);
bool trade_planning_is_automatic(void);
int trade_planning_set_automatic(bool automatic);

int trade_planning_dispatch_all(void);
bool trade_planning_auto_caravan(struct unit *punit);
void trade_planning_pulse(void);
bool trade_planning_handle_arrival(struct unit *actor_unit,
                                   struct city *target_city,
                                   const struct act_prob *act_probs);

const struct trade_planning_route_list *trade_planning_routes(void);
int trade_planning_location_count(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* FC__TRADEPLANNING_H */
