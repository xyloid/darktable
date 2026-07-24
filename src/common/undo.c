/*
    This file is part of darktable,
    Copyright (C) 2017-2024 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/undo.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "control/control.h"
#include <glib.h>   // for GList, gpointer, g_list_prepend
#include <stdlib.h> // for NULL, malloc, free
#include <sys/time.h>

const double MAX_TIME_PERIOD = 0.5; // in second

typedef struct dt_undo_item_t
{
  gpointer user_data;
  dt_undo_type_t type;
  dt_undo_data_t data;
  double ts;
  uint64_t coalesce_epoch;
  gboolean is_group;
  void (*undo)(gpointer user_data,
               dt_undo_type_t type,
               dt_undo_data_t data,
               dt_undo_action_t action,
               GList **imgs);
  void (*free_data)(gpointer data);
} dt_undo_item_t;

dt_undo_t *dt_undo_init(void)
{
  dt_undo_t *udata = malloc(sizeof(dt_undo_t));
  udata->undo_list = NULL;
  udata->redo_list = NULL;
  udata->disable_next_owners = NULL;

  pthread_mutexattr_t recursive_locking;
  pthread_mutexattr_init(&recursive_locking);
  pthread_mutexattr_settype(&recursive_locking, PTHREAD_MUTEX_RECURSIVE);
  dt_pthread_mutex_init(&udata->mutex, &recursive_locking);

  udata->group = DT_UNDO_NONE;
  udata->group_indent = 0;
  udata->group_start = NULL;
  udata->coalesce_epoch = 0;
  udata->isolated_group_active = FALSE;
  udata->isolated_group_committed = FALSE;
  udata->isolated_saved_group = DT_UNDO_NONE;
  udata->isolated_saved_group_indent = 0;
  udata->isolated_saved_group_start = NULL;
  dt_print(DT_DEBUG_UNDO, "[undo] init");
  return udata;
}

#define LOCK    dt_pthread_mutex_lock(&self->mutex);
#define UNLOCK  dt_pthread_mutex_unlock(&self->mutex)

static GList *_undo_disable_next_owner_link(dt_undo_t *self,
                                            const pthread_t thread)
{
  for(GList *owners = self->disable_next_owners;
      owners;
      owners = g_list_next(owners))
  {
    const pthread_t *owner = owners->data;
    if(pthread_equal(*owner, thread)) return owners;
  }
  return NULL;
}

static void _undo_clear_disable_next(dt_undo_t *self)
{
  g_list_free_full(self->disable_next_owners, free);
  self->disable_next_owners = NULL;
}

void dt_undo_disable_next(dt_undo_t *self)
{
  if(!self) return;
  LOCK;
  const pthread_t thread = pthread_self();
  if(!_undo_disable_next_owner_link(self, thread))
  {
    pthread_t *owner = malloc(sizeof(*owner));
    *owner = thread;
    self->disable_next_owners =
      g_list_prepend(self->disable_next_owners, owner);
  }
  dt_print(DT_DEBUG_UNDO, "[undo] disable next");
  UNLOCK;
}

void dt_undo_cleanup(dt_undo_t *self)
{
  if(!self) return;
  LOCK;
  if(self->isolated_group_active)
  {
    assert(!self->isolated_group_active);
    UNLOCK;
    return;
  }
  UNLOCK;
  dt_undo_clear(self, DT_UNDO_ALL);
  dt_pthread_mutex_destroy(&self->mutex);
}

static void _free_undo_data(void *p)
{
  dt_undo_item_t *item = (dt_undo_item_t *)p;
  if(item->free_data) item->free_data(item->data);
  free(item);
}

static dt_undo_item_t *_undo_push_item(
  dt_undo_t *self,
  gpointer user_data,
  const dt_undo_type_t type,
  const dt_undo_data_t data,
  const gboolean is_group,
  void (*undo)(gpointer user_data,
               const dt_undo_type_t type,
               const dt_undo_data_t item,
               const dt_undo_action_t action,
               GList **imgs),
  void (*free_data)(gpointer data))
{
  dt_undo_item_t *item = malloc(sizeof(dt_undo_item_t));

  item->user_data = user_data;
  item->type      = type;
  item->data      = data;
  item->undo      = undo;
  item->free_data = free_data;
  item->ts        = dt_get_wtime();
  item->coalesce_epoch = self->coalesce_epoch;
  item->is_group  = is_group;

  self->undo_list = g_list_prepend(self->undo_list, (gpointer)item);

  // recording an undo data, invalidate all the redo
  g_list_free_full(self->redo_list, _free_undo_data);
  self->redo_list = NULL;

  dt_print(DT_DEBUG_UNDO, "[undo] record for type %d (length %d)",
           type, g_list_length(self->undo_list));
  return item;
}

static dt_undo_item_t *_undo_push_group(dt_undo_t *self,
                                        const dt_undo_type_t type)
{
  return _undo_push_item(self, NULL, type, NULL, TRUE, NULL, NULL);
}

static void _undo_seal_group_segment(dt_undo_t *self,
                                     const dt_undo_type_t type,
                                     gpointer *group_start)
{
  dt_undo_item_t *start = *group_start;
  if(!start) return;

  GList *start_link = g_list_find(self->undo_list, start);
  if(start_link && start_link == self->undo_list)
  {
    self->undo_list =
      g_list_delete_link(self->undo_list, start_link);
    _free_undo_data(start);
  }
  else if(start_link)
    _undo_push_group(self, type);

  // A missing marker is not owned by undo_list (for example, an older
  // traversal moved it to redo). Never free through a stale pointer; the
  // list that still owns the marker handles cleanup.
  *group_start = NULL;
}

static void _undo_commit_isolated_group(dt_undo_t *self)
{
  assert(self->isolated_group_active);
  assert(!self->isolated_group_committed);

  _undo_seal_group_segment(self, self->isolated_saved_group,
                           &self->isolated_saved_group_start);

  self->coalesce_epoch++;
  self->group_start = _undo_push_group(self, self->group);
  self->isolated_group_committed = TRUE;
}

static void _undo_record(dt_undo_t *self,
                         gpointer user_data,
                         const dt_undo_type_t type,
                         const dt_undo_data_t data,
                         void (*undo)(gpointer user_data,
                                      const dt_undo_type_t type,
                                      const dt_undo_data_t item,
                                      const dt_undo_action_t action,
                                      GList **imgs),
                         void (*free_data)(gpointer data))
{
  if(!self) return;

  LOCK;

  GList *disable_owner =
    _undo_disable_next_owner_link(self, pthread_self());
  if(disable_owner)
  {
    free(disable_owner->data);
    self->disable_next_owners =
      g_list_delete_link(self->disable_next_owners, disable_owner);
    if(free_data) free_data(data);
    dt_print(DT_DEBUG_UNDO, "[undo] record for type %d, disable next",
             type);
    UNLOCK;
    return;
  }

  if(self->isolated_group_active
     && !self->isolated_group_committed)
    _undo_commit_isolated_group(self);
  else if(self->group != DT_UNDO_NONE && !self->group_start)
    self->group_start = _undo_push_group(self, self->group);

  _undo_push_item(self, user_data, type, data, FALSE, undo, free_data);
  UNLOCK;
}

void dt_undo_start_group(dt_undo_t *self,
                         const dt_undo_type_t type)
{
  if(!self) return;

  LOCK;
  if(self->group == DT_UNDO_NONE)
  {
    dt_print(DT_DEBUG_UNDO, "[undo] start group for type %d", type);
    self->group = type;
    self->group_indent = 1;
    self->group_start = _undo_push_group(self, type);
  }
  else
    self->group_indent++;
  UNLOCK;
}

void dt_undo_end_group(dt_undo_t *self)
{
  if(!self) return;
  LOCK;
  assert(self->group_indent>0);
  assert(!self->isolated_group_active || self->group_indent > 1);
  self->group_indent--;
  if(self->group_indent == 0)
  {
    if(self->group_start)
      _undo_push_group(self, self->group);
    dt_print(DT_DEBUG_UNDO, "[undo] end group for type %d", self->group);
    self->group = DT_UNDO_NONE;
    self->group_start = NULL;
  }
  UNLOCK;
}

void dt_undo_start_recording(dt_undo_t *self)
{
  if(!self) return;
  LOCK;
}

void dt_undo_end_recording(dt_undo_t *self)
{
  if(!self) return;
  UNLOCK;
}

void dt_undo_start_isolated_group(dt_undo_t *self,
                                  const dt_undo_type_t type)
{
  if(!self) return;

  // Deliberately retained until dt_undo_end_isolated_group(). Recursive
  // acquisition lets the records inside this same-thread scope use the
  // ordinary public API while excluding interleaving writers.
  LOCK;
  assert(!self->isolated_group_active);
  self->isolated_group_active = TRUE;
  self->isolated_group_committed = FALSE;
  self->isolated_saved_group = self->group;
  self->isolated_saved_group_indent = self->group_indent;
  self->isolated_saved_group_start = self->group_start;
  self->group = type;
  self->group_indent = 1;
  self->group_start = NULL;
  dt_print(DT_DEBUG_UNDO, "[undo] start isolated group for type %d",
           type);
}

void dt_undo_end_isolated_group(dt_undo_t *self)
{
  if(!self) return;

  // This releases the acquisition retained by start; do not acquire an
  // additional level here.
  assert(self->isolated_group_active);
  assert(self->group_indent == 1);
  if(self->isolated_group_committed)
  {
    _undo_push_group(self, self->group);
    self->coalesce_epoch++;
  }

  self->group = self->isolated_saved_group;
  self->group_indent = self->isolated_saved_group_indent;
  self->group_start = self->isolated_group_committed
                        ? NULL
                        : self->isolated_saved_group_start;
  self->isolated_group_active = FALSE;
  self->isolated_group_committed = FALSE;
  self->isolated_saved_group = DT_UNDO_NONE;
  self->isolated_saved_group_indent = 0;
  self->isolated_saved_group_start = NULL;
  dt_print(DT_DEBUG_UNDO, "[undo] end isolated group");
  UNLOCK;
}

void dt_undo_record(dt_undo_t *self,
                    gpointer user_data,
                    dt_undo_type_t type,
                    dt_undo_data_t data,
                    void (*undo)(gpointer user_data,
                                 const dt_undo_type_t type,
                                 const dt_undo_data_t item,
                                 const dt_undo_action_t action,
                                 GList **imgs),
                    void (*free_data)(gpointer data))
{
  _undo_record(self, user_data, type, data, undo, free_data);
}

gint _images_list_cmp(gconstpointer a, gconstpointer b)
{
  return GPOINTER_TO_INT(a) - GPOINTER_TO_INT(b);
}

static void _undo_do_undo_redo(dt_undo_t *self,
                               const uint32_t filter,
                               const dt_undo_action_t action)
{
  if(!self) return;

  LOCK;
  if(self->isolated_group_active)
  {
    assert(!self->isolated_group_active);
    UNLOCK;
    return;
  }

  // An active ordinary group may span a long background job. Close only
  // its current list segment before traversal; keep the logical group open
  // and start a new segment lazily if that job records again.
  if(self->group != DT_UNDO_NONE)
    _undo_seal_group_segment(self, self->group, &self->group_start);

  // we take/remove item from the FROM list and add them into the TO list:
  GList **from = action == DT_ACTION_UNDO ? &self->undo_list : &self->redo_list;
  GList **to   = action == DT_ACTION_UNDO ? &self->redo_list : &self->undo_list;

  GList *imgs = NULL;

  // check for first item that is matching the given pattern

  dt_print(DT_DEBUG_UNDO,
           "[undo] action %s for %d (from length %d -> to length %d)",
           action == DT_ACTION_UNDO ? "UNDO" : "DO",
           filter,
           g_list_length(*from),
           g_list_length(*to));

  for(GList *l = *from; l; l = g_list_next(l))
  {
    dt_undo_item_t *item = l->data;

    if(item->type & filter)
    {
      if(item->is_group)
      {
        gboolean is_group = FALSE;

        GList *next = g_list_next(l);

        //  first move the group item into the TO list
        *from = g_list_remove(*from, item);
        *to   = g_list_prepend(*to, item);

        while((l = next) && !is_group)
        {
          item = (dt_undo_item_t *)l->data;
          next = g_list_next(l);

          //  first remove element from FROM list
          *from = g_list_remove(*from, item);

          //  callback with undo or redo data
          if(item->is_group)
            is_group = TRUE;
          else
            item->undo(item->user_data, item->type, item->data, action, &imgs);

          //  add old position back into the TO list
          *to = g_list_prepend(*to, item);
        }
      }
      else
      {
        const double first_item_ts = item->ts;
        const uint64_t first_item_epoch = item->coalesce_epoch;
        gboolean in_group = FALSE;

        //  when found, handle all items of the same type and in the same time period

        do
        {
          GList *next = g_list_next(l);

          //  first remove element from FROM list
          *from = g_list_remove(*from, item);

          if(item->is_group)
            in_group = !in_group;
          else
            //  callback with redo or redo data
            item->undo(item->user_data, item->type, item->data, action, &imgs);

          //  add old position back into the TO list
          *to = g_list_prepend(*to, item);

          l = next;
          if(l) item = (dt_undo_item_t *)l->data;
        } while(l
                && (item->type & filter)
                && (in_group
                    || (item->coalesce_epoch == first_item_epoch
                        && fabs(item->ts - first_item_ts)
                             < MAX_TIME_PERIOD)));
      }

      break;
    }
  }
  UNLOCK;

  if(imgs)
  {
    imgs = g_list_sort(imgs, _images_list_cmp);
    // remove duplicates
    for(const GList *img = imgs; img; img = g_list_next(img))
    {
      // udpate xmp is done via set_change_timestamp
      dt_image_cache_set_change_timestamp(GPOINTER_TO_INT(img->data));
      while(img->next && img->data == img->next->data)
        imgs = g_list_delete_link(imgs, img->next);
    }
  }

  dt_collection_update_query(darktable.collection,
                             DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF, imgs);
}

void dt_undo_do_redo(dt_undo_t *self, const uint32_t filter)
{
  dt_gui_cursor_set_busy();
  _undo_do_undo_redo(self, filter, DT_ACTION_REDO);
  dt_gui_cursor_clear_busy();
}

void dt_undo_do_undo(dt_undo_t *self, const uint32_t filter)
{
  dt_gui_cursor_set_busy();
  _undo_do_undo_redo(self, filter, DT_ACTION_UNDO);
  dt_gui_cursor_clear_busy();
}

static void _undo_clear_list(GList **list, const uint32_t filter)
{
  // check for first item that is matching the given pattern

  GList *next;
  for(GList *l = *list; l; l = next)
  {
    dt_undo_item_t *item = l->data;
    next = g_list_next(l); // get next node now, because we may delete the current one
    if(item->type & filter)
    {
      //  remove this element
      *list = g_list_remove(*list, item);
      _free_undo_data((void *)item);
    }
  };

  dt_print(DT_DEBUG_UNDO, "[undo] clear list for %d (length %d)",
           filter, g_list_length(*list));
}

void dt_undo_clear(dt_undo_t *self, uint32_t filter)
{
  if(!self) return;

  LOCK;
  if(self->isolated_group_active)
  {
    assert(!self->isolated_group_active);
    UNLOCK;
    return;
  }
  _undo_clear_disable_next(self);
  _undo_clear_list(&self->undo_list, filter);
  _undo_clear_list(&self->redo_list, filter);
  self->undo_list = NULL;
  self->redo_list = NULL;
  self->group_start = NULL;
  // Preserve clear's reset contract even if a recursive free callback armed
  // a fresh token while the lists were being released.
  _undo_clear_disable_next(self);
  UNLOCK;
}

static void _undo_iterate(GList *list,
                          const uint32_t filter,
                          gpointer user_data,
                          void (*apply)(gpointer user_data,
                                        const dt_undo_type_t type,
                                        const dt_undo_data_t item))
{
  // check for first item that is matching the given pattern
  for(GList *l = list; l; l = g_list_next(l))
  {
    dt_undo_item_t *item = l->data;
    if(!item->is_group && (item->type & filter))
    {
      apply(user_data, item->type, item->data);
    }
  };
}

void dt_undo_iterate(dt_undo_t *self,
                     const uint32_t filter,
                     gpointer user_data,
                     void (*apply)(gpointer user_data,
                                            const dt_undo_type_t type,
                                            const dt_undo_data_t item))
{
  if(!self) return;
  LOCK;
  if(self->isolated_group_active)
  {
    assert(!self->isolated_group_active);
    UNLOCK;
    return;
  }
  _undo_iterate(self->undo_list, filter, user_data, apply);
  _undo_iterate(self->redo_list, filter, user_data, apply);
  UNLOCK;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
