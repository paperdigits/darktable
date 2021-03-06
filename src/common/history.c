/*
    This file is part of darktable,
    copyright (c) 2010 henrik andersson,
    copyright (c) 2011-2012 johannes hanika

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

#include "common/history.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/mipmap_cache.h"
#include "common/tags.h"
#include "common/utility.h"
#include "common/collection.h"
#include "common/history_snapshot.h"
#include "common/undo.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/blend.h"
#include "develop/masks.h"

#define DT_IOP_ORDER_INFO (darktable.unmuted & DT_DEBUG_IOPORDER)

void dt_history_item_free(gpointer data)
{
  dt_history_item_t *item = (dt_history_item_t *)data;
  g_free(item->op);
  g_free(item->name);
  item->op = NULL;
  item->name = NULL;
  g_free(item);
}

static void remove_preset_flag(const int imgid)
{
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');

  // clear flag
  image->flags &= ~DT_IMAGE_AUTO_PRESETS_APPLIED;

  // write through to sql+xmp
  dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
}

void dt_history_delete_on_image_ext(int32_t imgid, gboolean undo)
{
  dt_undo_lt_history_t *hist = undo?dt_history_snapshot_item_init():NULL;

  if(undo)
  {
    hist->imgid = imgid;
    dt_history_snapshot_undo_create(hist->imgid, &hist->before, &hist->before_history_end);
  }

  dt_lock_image(imgid);

  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.history WHERE imgid = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.module_order WHERE imgid = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "UPDATE main.images SET history_end = 0, aspect_ratio = 0.0 WHERE id = ?1", -1, &stmt,
      NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.masks_history WHERE imgid = ?1", -1, &stmt,
                              NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  remove_preset_flag(imgid);

  /* if current image in develop reload history */
  if(dt_dev_is_current_image(darktable.develop, imgid)) dt_dev_reload_history_items(darktable.develop);

  /* make sure mipmaps are recomputed */
  dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);
  dt_image_reset_final_size(imgid);

  /* remove darktable|style|* tags */
  dt_tag_detach_by_string("darktable|style%", imgid, FALSE, FALSE);
  dt_tag_detach_by_string("darktable|changed", imgid, FALSE, FALSE);

  dt_unlock_image(imgid);

  if(undo)
  {
    dt_history_snapshot_undo_create(hist->imgid, &hist->after, &hist->after_history_end);

    dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);
    dt_undo_record(darktable.undo, NULL, DT_UNDO_LT_HISTORY, (dt_undo_data_t)hist,
                   dt_history_snapshot_undo_pop, dt_history_snapshot_undo_lt_history_data_free);
    dt_undo_end_group(darktable.undo);
  }
}

void dt_history_delete_on_image(int32_t imgid)
{
  dt_history_delete_on_image_ext(imgid, TRUE);
}

void dt_history_delete_on_selection()
{
  sqlite3_stmt *stmt;

  dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images",
                              -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int imgid = sqlite3_column_int(stmt, 0);
    dt_undo_lt_history_t *hist = dt_history_snapshot_item_init();

    hist->imgid = imgid;
    dt_history_snapshot_undo_create(hist->imgid, &hist->before, &hist->before_history_end);

    dt_history_delete_on_image_ext(imgid, FALSE);

    dt_history_snapshot_undo_create(hist->imgid, &hist->after, &hist->after_history_end);
    dt_undo_record(darktable.undo, NULL, DT_UNDO_LT_HISTORY, (dt_undo_data_t)hist,
                   dt_history_snapshot_undo_pop, dt_history_snapshot_undo_lt_history_data_free);

    /* update the aspect ratio if the current sorting is based on aspect ratio, otherwise the aspect ratio will be
       recalculated when the mimpap will be recreated */
    if (darktable.collection->params.sort == DT_COLLECTION_SORT_ASPECT_RATIO)
      dt_image_set_aspect_ratio(imgid);
  }
  sqlite3_finalize(stmt);

  dt_undo_end_group(darktable.undo);
}

int dt_history_load_and_apply(const int imgid, gchar *filename, int history_only)
{
  dt_lock_image(imgid);
  dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
  if(img)
  {
    dt_undo_lt_history_t *hist = dt_history_snapshot_item_init();
    hist->imgid = imgid;
    dt_history_snapshot_undo_create(hist->imgid, &hist->before, &hist->before_history_end);

    if(dt_exif_xmp_read(img, filename, history_only))
    {
      dt_unlock_image(imgid);
      return 1;
    }
    dt_history_snapshot_undo_create(hist->imgid, &hist->after, &hist->after_history_end);
    dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);
    dt_undo_record(darktable.undo, NULL, DT_UNDO_LT_HISTORY, (dt_undo_data_t)hist,
                   dt_history_snapshot_undo_pop, dt_history_snapshot_undo_lt_history_data_free);
    dt_undo_end_group(darktable.undo);

    /* if current image in develop reload history */
    if(dt_dev_is_current_image(darktable.develop, imgid)) dt_dev_reload_history_items(darktable.develop);

    dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_SAFE);
    dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);
    dt_image_reset_final_size(imgid);
  }
  dt_unlock_image(imgid);
  return 0;
}

int dt_history_load_and_apply_on_selection(gchar *filename)
{
  int res = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images",
                              -1, &stmt, NULL);
  dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int imgid = sqlite3_column_int(stmt, 0);
    if(dt_history_load_and_apply(imgid, filename, 1)) res = 1;
  }
  dt_undo_end_group(darktable.undo);
  sqlite3_finalize(stmt);
  return res;
}

// returns the first history item with hist->module == module
static dt_dev_history_item_t *_search_history_by_module(dt_develop_t *dev, dt_iop_module_t *module)
{
  dt_dev_history_item_t *hist_mod = NULL;
  GList *history = g_list_first(dev->history);
  while(history)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);

    if(hist->module == module)
    {
      hist_mod = hist;
      break;
    }
    history = g_list_next(history);
  }
  return hist_mod;
}

// returns the first history item with corresponding module->op
static dt_dev_history_item_t *_search_history_by_op(dt_develop_t *dev, dt_iop_module_t *module)
{
  dt_dev_history_item_t *hist_mod = NULL;
  GList *history = g_list_first(dev->history);
  while(history)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);

    if(strcmp(hist->module->op, module->op) == 0)
    {
      hist_mod = hist;
      break;
    }
    history = g_list_next(history);
  }
  return hist_mod;
}

// returns the module on modules_list that is equal to module
// used to check if module exists on the list
static dt_iop_module_t *_search_list_iop_by_module(GList *modules_list, dt_iop_module_t *module)
{
  dt_iop_module_t *mod_ret = NULL;
  GList *modules = g_list_first(modules_list);
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)(modules->data);

    if(mod == module)
    {
      mod_ret = mod;
      break;
    }
    modules = g_list_next(modules);
  }
  return mod_ret;
}

// fills used with formid, if it is a group it recurs and fill all sub-forms
static void _fill_used_forms(GList *forms_list, int formid, int *used, int nb)
{
  // first, we search for the formid in used table
  for(int i = 0; i < nb; i++)
  {
    if(used[i] == 0)
    {
      // we store the formid
      used[i] = formid;
      break;
    }
    if(used[i] == formid) break;
  }

  // if the form is a group, we iterate through the sub-forms
  dt_masks_form_t *form = dt_masks_get_from_id_ext(forms_list, formid);
  if(form && (form->type & DT_MASKS_GROUP))
  {
    GList *grpts = g_list_first(form->points);
    while(grpts)
    {
      dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)grpts->data;
      _fill_used_forms(forms_list, grpt->formid, used, nb);
      grpts = g_list_next(grpts);
    }
  }
}

// dev_src is used only to copy masks, if no mask will be copied it can be null
int dt_history_merge_module_into_history(dt_develop_t *dev_dest, dt_develop_t *dev_src, dt_iop_module_t *mod_src, GList **_modules_used, const int append)
{
  int module_added = 1;
  GList *modules_used = *_modules_used;
  dt_iop_module_t *module = NULL;
  dt_iop_module_t *mod_replace = NULL;

  // one-instance modules always replace the existing one
  if(mod_src->flags() & IOP_FLAGS_ONE_INSTANCE)
  {
    mod_replace = dt_iop_get_module_by_op_priority(dev_dest->iop, mod_src->op, -1);
    if(mod_replace == NULL)
    {
      fprintf(stderr, "[dt_history_merge_module_into_history] can't find single instance module %s\n",
              mod_src->op);
      module_added = 0;
    }
  }

  if(module_added && mod_replace == NULL && !append)
  {
    // we haven't found a module to replace
    // check if there's a module with the same (operation, multi_name) on dev->iop
    GList *modules_dest = g_list_first(dev_dest->iop);
    while(modules_dest)
    {
      dt_iop_module_t *mod_dest = (dt_iop_module_t *)modules_dest->data;

      if(strcmp(mod_src->op, mod_dest->op) == 0 && strcmp(mod_src->multi_name, mod_dest->multi_name) == 0)
      {
        // but only if it hasn't been used already
        if(_search_list_iop_by_module(modules_used, mod_dest) == NULL)
        {
          // we will replace this module
          modules_used = g_list_append(modules_used, mod_dest);
          mod_replace = mod_dest;
          break;
        }
      }
      modules_dest = g_list_next(modules_dest);
    }
  }

  if(module_added && mod_replace == NULL)
  {
    // we haven't found a module to replace, so we will create a new instance
    // but if there's an un-used instance on dev->iop we will use that

    if(_search_history_by_op(dev_dest, mod_src) == NULL)
    {
      // there should be only one instance of this iop (since is un-used)
      mod_replace = dt_iop_get_module_by_op_priority(dev_dest->iop, mod_src->op, -1);
      if(mod_replace == NULL)
      {
        fprintf(stderr, "[dt_history_merge_module_into_history] can't find base instance module %s\n", mod_src->op);
        module_added = 0;
      }
    }
  }

  if(module_added)
  {
    // if we are creating a new instance, create a new module
    if(mod_replace == NULL)
    {
      dt_iop_module_t *base = dt_iop_get_module_by_op_priority(dev_dest->iop, mod_src->op, -1);
      module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
      if(dt_iop_load_module(module, base->so, dev_dest))
      {
        fprintf(stderr, "[dt_history_merge_module_into_history] can't load module %s\n", mod_src->op);
        module_added = 0;
      }
      else
      {
        module->instance = mod_src->instance;
        module->multi_priority = mod_src->multi_priority;
        module->iop_order = dt_ioppr_get_iop_order(dev_dest->iop_order_list, module->op, module->multi_priority);
      }
    }
    else
    {
      module = mod_replace;
    }

    module->enabled = mod_src->enabled;
    g_strlcpy(module->multi_name, mod_src->multi_name, sizeof(module->multi_name));

    memcpy(module->params, mod_src->params, module->params_size);
    if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
    {
      memcpy(module->blend_params, mod_src->blend_params, sizeof(dt_develop_blend_params_t));
      module->blend_params->mask_id = mod_src->blend_params->mask_id;
    }
  }

  // we have the module, we will use the source module iop_order unless there's already
  // a module with that order
  if(module_added)
  {
    dt_iop_module_t *module_duplicate = NULL;
    // check if there's a module with the same iop_order
    GList *modules_dest = g_list_first(dev_dest->iop);
    while(modules_dest)
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)(modules_dest->data);

      if(module_duplicate != NULL)
      {
        module_duplicate = mod;
        break;
      }
      if(mod->iop_order == mod_src->iop_order && mod != module)
      {
        module_duplicate = mod;
      }

      modules_dest = g_list_next(modules_dest);
    }

    // do some checking...
    if(mod_src->iop_order <= 0.0 || mod_src->iop_order == INT_MAX)
      fprintf(stderr, "[dt_history_merge_module_into_history] invalid source module %s %s(%d)(%i)\n",
          mod_src->op, mod_src->multi_name, mod_src->iop_order, mod_src->multi_priority);
    if(module_duplicate && (module_duplicate->iop_order <= 0.0 || module_duplicate->iop_order == INT_MAX))
      fprintf(stderr, "[dt_history_merge_module_into_history] invalid duplicate module module %s %s(%d)(%i)\n",
          module_duplicate->op, module_duplicate->multi_name, module_duplicate->iop_order, module_duplicate->multi_priority);
    if(module->iop_order <= 0.0 || module->iop_order == INT_MAX)
      fprintf(stderr, "[dt_history_merge_module_into_history] invalid iop_order for module %s %s(%d)(%i)\n",
          module->op, module->multi_name, module->iop_order, module->multi_priority);

    // if this is a new module just add it to the list
    if(mod_replace == NULL)
      dev_dest->iop = g_list_insert_sorted(dev_dest->iop, module, dt_sort_iop_by_order);
    else
      dev_dest->iop = g_list_sort(dev_dest->iop, dt_sort_iop_by_order);
  }

  // and we add it to history
  if(module_added)
  {
    // copy masks
    guint nbf = 0;
    int *forms_used_replace = NULL;

    if(dev_src)
    {
      // we will copy only used forms
      // record the masks used by this module
      if(mod_src->flags() & IOP_FLAGS_SUPPORTS_BLENDING && mod_src->blend_params->mask_id > 0)
      {
        nbf = g_list_length(dev_src->forms);
        forms_used_replace = calloc(nbf, sizeof(int));

        _fill_used_forms(dev_src->forms, mod_src->blend_params->mask_id, forms_used_replace, nbf);

        // now copy masks
        for(int i = 0; i < nbf && forms_used_replace[i] > 0; i++)
        {
          dt_masks_form_t *form = dt_masks_get_from_id_ext(dev_src->forms, forms_used_replace[i]);
          if(form)
          {
            // check if the form already exists in dest image
            // if so we'll remove it, so it is replaced
            dt_masks_form_t *form_dest = dt_masks_get_from_id_ext(dev_dest->forms, forms_used_replace[i]);
            if(form_dest)
            {
              dev_dest->forms = g_list_remove(dev_dest->forms, form_dest);
              // and add it to allforms to cleanup
              dev_dest->allforms = g_list_append(dev_dest->allforms, form_dest);
            }

            // and add it to dest image
            dt_masks_form_t *form_new = dt_masks_dup_masks_form(form);
            dev_dest->forms = g_list_append(dev_dest->forms, form_new);
          }
          else
            fprintf(stderr, "[dt_history_merge_module_into_history] form %i not found in source image\n", forms_used_replace[i]);
        }
      }
    }

    if(nbf > 0 && forms_used_replace[0] > 0)
      dt_dev_add_masks_history_item_ext(dev_dest, module, FALSE, TRUE);
    else
      dt_dev_add_history_item_ext(dev_dest, module, FALSE, TRUE);

    dt_ioppr_resync_modules_order(dev_dest);

    dt_dev_pop_history_items_ext(dev_dest, dev_dest->history_end);

    if(forms_used_replace) free(forms_used_replace);
  }

  *_modules_used = modules_used;

  return module_added;
}

static int _history_copy_and_paste_on_image_merge(int32_t imgid, int32_t dest_imgid, GList *ops)
{
  GList *modules_used = NULL;

  dt_develop_t _dev_src = { 0 };
  dt_develop_t _dev_dest = { 0 };

  dt_develop_t *dev_src = &_dev_src;
  dt_develop_t *dev_dest = &_dev_dest;

  // we will do the copy/paste on memory so we can deal with masks
  dt_dev_init(dev_src, FALSE);
  dt_dev_init(dev_dest, FALSE);

  dev_src->iop = dt_iop_load_modules_ext(dev_src, TRUE);
  dev_dest->iop = dt_iop_load_modules_ext(dev_dest, TRUE);

  dt_dev_read_history_ext(dev_src, imgid, TRUE);

  // This prepends the default modules and converts just in case it's an empty history
  dt_dev_read_history_ext(dev_dest, dest_imgid, TRUE);

  dt_ioppr_check_iop_order(dev_src, imgid, "_history_copy_and_paste_on_image_merge ");
  dt_ioppr_check_iop_order(dev_dest, dest_imgid, "_history_copy_and_paste_on_image_merge ");

  dt_dev_pop_history_items_ext(dev_src, dev_src->history_end);
  dt_dev_pop_history_items_ext(dev_dest, dev_dest->history_end);

  dt_ioppr_check_iop_order(dev_src, imgid, "_history_copy_and_paste_on_image_merge 1");
  dt_ioppr_check_iop_order(dev_dest, dest_imgid, "_history_copy_and_paste_on_image_merge 1");

  GList *mod_list = NULL;

  if(ops)
  {
    if (DT_IOP_ORDER_INFO) fprintf(stderr," selected ops");
    // copy only selected history entries
    GList *l = g_list_last(ops);
    while(l)
    {
      const unsigned int num = GPOINTER_TO_UINT(l->data);

      const dt_dev_history_item_t *hist = g_list_nth_data(dev_src->history, num);

      if(hist)
      {
        if (!dt_iop_is_hidden(hist->module))
        {
          if (DT_IOP_ORDER_INFO)
            fprintf(stderr,"\n  module %20s, multiprio %i",  hist->module->op, hist->module->multi_priority);

          mod_list = g_list_append(mod_list, hist->module);
        }
      }

      l = g_list_previous(l);
    }
  }
  else
  {
    if (DT_IOP_ORDER_INFO) fprintf(stderr," all modules");
    // we will copy all modules
    GList *modules_src = g_list_first(dev_src->iop);
    while(modules_src)
    {
      dt_iop_module_t *mod_src = (dt_iop_module_t *)(modules_src->data);

      // copy from history only if
      if((_search_history_by_module(dev_src, mod_src) != NULL) // module is in history of source image
         && !(mod_src->default_enabled && mod_src->enabled
              && !memcmp(mod_src->params, mod_src->default_params, mod_src->params_size) // it's not a enabled by default module with unmodified settings
              && !dt_iop_is_hidden(mod_src))
        )
      {
        mod_list = g_list_append(mod_list, mod_src);
      }

      modules_src = g_list_next(modules_src);
    }
  }
  if (DT_IOP_ORDER_INFO) fprintf(stderr,"\nvvvvv\n");

  // update iop-order list to have entries for the new modules
  dt_ioppr_update_for_modules(dev_dest, mod_list, FALSE);

  GList *l = mod_list;
  while(l)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)l->data;
    dt_history_merge_module_into_history(dev_dest, dev_src, mod, &modules_used, FALSE);
    l = g_list_next(l);
  }

  // update iop-order list to have entries for the new modules
  dt_ioppr_update_for_modules(dev_dest, mod_list, FALSE);

  dt_ioppr_check_iop_order(dev_dest, dest_imgid, "_history_copy_and_paste_on_image_merge 2");

  // write history and forms to db
  dt_dev_write_history_ext(dev_dest, dest_imgid);

  dt_dev_cleanup(dev_src);
  dt_dev_cleanup(dev_dest);

  g_list_free(modules_used);

  return 0;
}

static int _history_copy_and_paste_on_image_overwrite(int32_t imgid, int32_t dest_imgid, GList *ops)
{
  int ret_val = 0;
  sqlite3_stmt *stmt;

  // replace history stack
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.history WHERE imgid = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // and shapes
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.masks_history WHERE imgid = ?1", -1, &stmt,
                              NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "UPDATE main.images SET history_end = 0, aspect_ratio = 0.0 WHERE id = ?1", -1, &stmt,
      NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // the user wants an exact duplicate of the history, so just copy the db
  if(!ops)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT INTO main.history "
                                "            (imgid,num,module,operation,op_params,enabled,blendop_params, "
                                "             blendop_version,multi_priority,multi_name)"
                                " SELECT ?1,num,module,operation,op_params,enabled,blendop_params, "
                                "        blendop_version,multi_priority,multi_name "
                                " FROM main.history"
                                " WHERE imgid=?2"
                                " ORDER BY num",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT INTO main.masks_history "
                                "           (imgid, num, formid, form, name, version, points, points_count, source)"
                                " SELECT ?1, num, formid, form, name, version, points, points_count, source "
                                " FROM main.masks_history"
                                " WHERE imgid = ?2",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    int history_end = 0;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT history_end FROM main.images WHERE id = ?1",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      if(sqlite3_column_type(stmt, 0) != SQLITE_NULL)
        history_end = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "UPDATE main.images SET history_end = ?2"
                                " WHERE id = ?1",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, history_end);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // and finaly copy the module order

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT OR REPLACE INTO main.module_order (imgid, iop_list, version)"
                                " SELECT ?2, iop_list, version"
                                "   FROM module_order"
                                "   WHERE imgid = ?1",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, dest_imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  else
  {
    // since the history and masks where deleted we can do a merge
    ret_val = _history_copy_and_paste_on_image_merge(imgid, dest_imgid, ops);
  }

  return ret_val;
}

int dt_history_copy_and_paste_on_image(int32_t imgid, int32_t dest_imgid, gboolean merge, GList *ops)
{
  if(imgid == dest_imgid) return 1;

  if(imgid == -1)
  {
    dt_control_log(_("you need to copy history from an image before you paste it onto another"));
    return 1;
  }

  dt_lock_image_pair(imgid,dest_imgid);

  // be sure the current history is written before pasting some other history data
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view((dt_view_t *)cv) == DT_VIEW_DARKROOM) dt_dev_write_history(darktable.develop);

  dt_undo_lt_history_t *hist = dt_history_snapshot_item_init();
  hist->imgid = dest_imgid;
  dt_history_snapshot_undo_create(hist->imgid, &hist->before, &hist->before_history_end);

  int ret_val = 0;
  if(merge)
    ret_val = _history_copy_and_paste_on_image_merge(imgid, dest_imgid, ops);
  else
    ret_val = _history_copy_and_paste_on_image_overwrite(imgid, dest_imgid, ops);

  dt_history_snapshot_undo_create(hist->imgid, &hist->after, &hist->after_history_end);
  dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);
  dt_undo_record(darktable.undo, NULL, DT_UNDO_LT_HISTORY, (dt_undo_data_t)hist,
                 dt_history_snapshot_undo_pop, dt_history_snapshot_undo_lt_history_data_free);
  dt_undo_end_group(darktable.undo);

  /* attach changed tag reflecting actual change */
  guint tagid = 0;
  dt_tag_new("darktable|changed", &tagid);
  dt_tag_attach(tagid, dest_imgid, FALSE, FALSE);

  /* if current image in develop reload history */
  if(dt_dev_is_current_image(darktable.develop, dest_imgid))
  {
    dt_dev_reload_history_items(darktable.develop);
    dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));
  }

  /* update xmp file */
  dt_image_synch_xmp(dest_imgid);

  dt_mipmap_cache_remove(darktable.mipmap_cache, dest_imgid);
  dt_image_reset_final_size(imgid);

  /* update the aspect ratio. recompute only if really needed for performance reasons */
  if(darktable.collection->params.sort == DT_COLLECTION_SORT_ASPECT_RATIO)
    dt_image_set_aspect_ratio(dest_imgid);
  else
    dt_image_reset_aspect_ratio(dest_imgid);

  dt_unlock_image_pair(imgid,dest_imgid);

  return ret_val;
}

GList *dt_history_get_items(int32_t imgid, gboolean enabled)
{
  GList *result = NULL;
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT num, operation, enabled, multi_name"
                              " FROM main.history"
                              " WHERE imgid=?1 AND num IN (SELECT MAX(num) FROM main.history hst2 WHERE hst2.imgid=?1 AND "
                              "       hst2.operation=main.history.operation GROUP BY multi_priority) "
                              " ORDER BY num DESC",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(strcmp((const char*)sqlite3_column_text(stmt, 1), "mask_manager") == 0) continue;

    const int is_active = sqlite3_column_int(stmt, 2);

    if(enabled == FALSE || is_active)
    {
      char name[512] = { 0 };
      dt_history_item_t *item = g_malloc(sizeof(dt_history_item_t));
      item->num = sqlite3_column_int(stmt, 0);
      char *mname = g_strdup((gchar *)sqlite3_column_text(stmt, 3));
      if(enabled)
      {
        if(strcmp(mname, "0") == 0)
          g_snprintf(name, sizeof(name), "%s",
                     dt_iop_get_localized_name((char *)sqlite3_column_text(stmt, 1)));
        else
          g_snprintf(name, sizeof(name), "%s %s",
                     dt_iop_get_localized_name((char *)sqlite3_column_text(stmt, 1)),
                     (char *)sqlite3_column_text(stmt, 3));
      }
      else
      {
        if(strcmp(mname, "0") == 0)
          g_snprintf(name, sizeof(name), "%s (%s)",
                     dt_iop_get_localized_name((char *)sqlite3_column_text(stmt, 1)),
                     (is_active != 0) ? _("on") : _("off"));
        g_snprintf(name, sizeof(name), "%s %s (%s)",
                   dt_iop_get_localized_name((char *)sqlite3_column_text(stmt, 1)),
                   (char *)sqlite3_column_text(stmt, 3), (is_active != 0) ? _("on") : _("off"));
      }
      item->name = g_strdup(name);
      item->op = g_strdup((gchar *)sqlite3_column_text(stmt, 1));
      result = g_list_append(result, item);

      g_free(mname);
    }
  }
  sqlite3_finalize(stmt);
  return result;
}

char *dt_history_get_items_as_string(int32_t imgid)
{
  GList *items = NULL;
  const char *onoff[2] = { _("off"), _("on") };
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT operation, enabled, multi_name FROM main.history WHERE imgid=?1 ORDER BY num DESC", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  // collect all the entries in the history from the db
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    char *name = NULL, *multi_name = NULL;
    const char *mn = (char *)sqlite3_column_text(stmt, 2);
    if(mn && *mn && g_strcmp0(mn, " ") != 0 && g_strcmp0(mn, "0") != 0)
      multi_name = g_strconcat(" ", sqlite3_column_text(stmt, 2), NULL);
    name = g_strconcat(dt_iop_get_localized_name((char *)sqlite3_column_text(stmt, 0)),
                       multi_name ? multi_name : "", " (",
                       (sqlite3_column_int(stmt, 1) == 0) ? onoff[0] : onoff[1], ")", NULL);
    items = g_list_append(items, name);
    g_free(multi_name);
  }
  sqlite3_finalize(stmt);
  char *result = dt_util_glist_to_str("\n", items);
  g_list_free_full(items, g_free);
  return result;
}

int dt_history_copy_and_paste_on_selection(int32_t imgid, gboolean merge, GList *ops)
{
  if(imgid < 0) return 1;

  int res = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT imgid FROM main.selected_images WHERE imgid != ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);
    do
    {
      /* get imgid of selected image */
      int32_t dest_imgid = sqlite3_column_int(stmt, 0);

      /* paste history stack onto image id */
      dt_history_copy_and_paste_on_image(imgid, dest_imgid, merge, ops);

    } while(sqlite3_step(stmt) == SQLITE_ROW);
    dt_undo_end_group(darktable.undo);
  }
  else
    res = 1;

  sqlite3_finalize(stmt);
  return res;
}

void dt_history_set_compress_problem(int32_t imgid, gboolean set)
{
  guint tagid = 0;
  char tagname[64];
  snprintf(tagname, sizeof(tagname), "darktable|problem|history-compress");
  dt_tag_new(tagname, &tagid);
  if (set)
    dt_tag_attach(tagid, imgid, FALSE, FALSE);
  else
    dt_tag_detach(tagid, imgid, FALSE, FALSE);
}

static int dt_history_end_attop(int32_t imgid)
{
  int size=0;
  int end=0;
  sqlite3_stmt *stmt;

  // get highest num in history
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "SELECT MAX(num) FROM main.history WHERE imgid=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  if (sqlite3_step(stmt) == SQLITE_ROW)
    size = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // get history_end
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "SELECT history_end FROM main.images WHERE id=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if (sqlite3_step(stmt) == SQLITE_ROW)
    end = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // fprintf(stderr,"\ndt_history_end_attop for image %i: size %i, end %i",imgid,size,end);

  // a special case right after removing all history
  // It must be absolutely fresh and untouched so history_end is always on top
  if ((size==0) && (end==0)) return -1;

  // return 1 if end is larger than size
  if (end > size) return 1;

  // no compression as history_end is right in the middle of stack
  return 0;
}


/* Please note: dt_history_compress_on_image
  - is used in lighttable and darkroom mode
  - It compresses history *exclusively* in the database and does *not* touch anything on the history stack
*/
void dt_history_compress_on_image(int32_t imgid)
{
  dt_lock_image(imgid);
  sqlite3_stmt *stmt;

  // get history_end for image
  int my_history_end = 0;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "SELECT history_end FROM main.images WHERE id=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  if (sqlite3_step(stmt) == SQLITE_ROW)
    my_history_end = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  if (my_history_end == 0)
  {
    dt_history_delete_on_image(imgid);
    dt_unlock_image(imgid);
    return;
  }

  int masks_count = 0;
  const char *op_mask_manager = "mask_manager";
  gboolean manager_position = FALSE;

  // We must know for sure whether there is a mask manager at slot 0 in history
  // because only if this is **not** true history nums and history_end must be increased
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "SELECT COUNT(*) FROM main.history WHERE imgid = ?1 AND operation = ?2 AND num = 0", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, op_mask_manager, -1, SQLITE_TRANSIENT);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if (sqlite3_column_int(stmt, 0) == 1) manager_position = TRUE;
  }
  sqlite3_finalize(stmt);

  // compress history, keep disabled modules as documented
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.history"
                              " WHERE imgid = ?1 AND num NOT IN"
                              "   (SELECT MAX(num) FROM main.history"
                              "     WHERE imgid = ?1 AND num < ?2"
                              "     GROUP BY operation, multi_priority)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, my_history_end);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // delete all mask_manager entries
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "DELETE FROM main.history WHERE imgid = ?1 AND operation = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, op_mask_manager, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // compress masks history
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.masks_history WHERE imgid = ?1 AND num "
                                                             "NOT IN (SELECT MAX(num) FROM main.masks_history WHERE "
                                                             "imgid = ?1 AND num < ?2)", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, my_history_end);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // if there are masks create a mask manager entry, so we need to count them
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "SELECT COUNT(*) FROM main.masks_history WHERE imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW) masks_count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  if(masks_count > 0)
  {
    // Set num in masks history to make sure they are owned by the manager at slot 0.
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
      "UPDATE main.masks_history SET num = 0 WHERE imgid = ?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (!manager_position)
    {
      // make room for mask manager history entry
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
        "UPDATE main.history SET num=num+1 WHERE imgid = ?1", -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);

      // update history end
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
        "UPDATE main.images SET history_end = history_end+1 WHERE id = ?1", -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }

    // create a mask manager entry in history as first entry
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT INTO main.history (imgid, num, operation, op_params, module, enabled, "
                                "                          blendop_params, blendop_version, multi_priority, multi_name) "
                                " VALUES(?1, 0, ?2, NULL, 1, 0, NULL, 0, 0, '')",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, op_mask_manager, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  dt_unlock_image(imgid);
}

int dt_history_compress_on_selection()
{
  int uncompressed=0;

  // Get the list of selected images
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images", -1, &stmt, NULL);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int(stmt, 0);
    dt_lock_image(imgid);
    const int test = dt_history_end_attop(imgid);
    if (test == 1) // we do a compression and we know for sure history_end is at the top!
    {
      dt_history_set_compress_problem(imgid, FALSE);
      dt_history_compress_on_image(imgid);

      // now the modules are in right order but need renumbering to remove leaks
      int max=0;    // the maximum num in main_history for an image
      int size=0;   // the number of items in main_history for an image
      int done=0;   // used for renumbering index

      sqlite3_stmt *stmt2;

      // get highest num in history
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
        "SELECT MAX(num) FROM main.history WHERE imgid=?1", -1, &stmt2, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, imgid);
      if (sqlite3_step(stmt2) == SQLITE_ROW)
        max = sqlite3_column_int(stmt2, 0);
      sqlite3_finalize(stmt2);

      // get number of items in main.history
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
        "SELECT COUNT(*) FROM main.history WHERE imgid = ?1", -1, &stmt2, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, imgid);
      if(sqlite3_step(stmt2) == SQLITE_ROW)
        size = sqlite3_column_int(stmt2, 0);
      sqlite3_finalize(stmt2);

      if ((size>0) && (max>0))
      {
        for (int index=0;index<(max+1);index++)
        {
          sqlite3_stmt *stmt3;
          DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
            "SELECT num FROM main.history WHERE imgid=?1 AND num=?2", -1, &stmt3, NULL);
          DT_DEBUG_SQLITE3_BIND_INT(stmt3, 1, imgid);
          DT_DEBUG_SQLITE3_BIND_INT(stmt3, 2, index);
          if (sqlite3_step(stmt3) == SQLITE_ROW)
          {
            sqlite3_stmt *stmt4;
            // step by step set the correct num
            DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
              "UPDATE main.history SET num = ?3 WHERE imgid = ?1 AND num = ?2", -1, &stmt4, NULL);
            DT_DEBUG_SQLITE3_BIND_INT(stmt4, 1, imgid);
            DT_DEBUG_SQLITE3_BIND_INT(stmt4, 2, index);
            DT_DEBUG_SQLITE3_BIND_INT(stmt4, 3, done);
            sqlite3_step(stmt4);
            sqlite3_finalize(stmt4);

            done++;
          }
          sqlite3_finalize(stmt3);
        }
      }
      // update history end
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
        "UPDATE main.images SET history_end = ?2 WHERE id = ?1", -1, &stmt2, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, imgid);
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 2, done);
      sqlite3_step(stmt2);
      sqlite3_finalize(stmt2);

      dt_image_write_sidecar_file(imgid);
    }
    if (test == 0) // no compression as history_end is right in the middle of history
    {
      uncompressed++;
      dt_history_set_compress_problem(imgid, TRUE);
    }
    if (test == -1)
      dt_history_set_compress_problem(imgid, FALSE);

    dt_unlock_image(imgid);
  }

  sqlite3_finalize(stmt);
  return uncompressed;
}

gboolean dt_history_check_module_exists(int32_t imgid, const char *operation)
{
  dt_lock_image(imgid);
  gboolean result = FALSE;
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get(darktable.db),
    "SELECT imgid FROM main.history WHERE imgid= ?1 AND operation = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, operation, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) == SQLITE_ROW) result = TRUE;
  sqlite3_finalize(stmt);

  dt_unlock_image(imgid);
  return result;
}

GList *dt_history_duplicate(GList *hist)
{
  GList *result = NULL;

  GList *h = g_list_first(hist);
  while(h)
  {
    const dt_dev_history_item_t *old = (dt_dev_history_item_t *)(h->data);

    dt_dev_history_item_t *new = (dt_dev_history_item_t *)malloc(sizeof(dt_dev_history_item_t));

    memcpy(new, old, sizeof(dt_dev_history_item_t));

    int32_t params_size = 0;
    if(old->module)
    {
      params_size = old->module->params_size;
    }
    else
    {
      dt_iop_module_t *base = dt_iop_get_module(old->op_name);
      if(base)
      {
        params_size = base->params_size;
      }
      else
      {
        // nothing else to do
        fprintf(stderr, "[_duplicate_history] can't find base module for %s\n", old->op_name);
      }
    }

    new->params = malloc(params_size);
    new->blend_params = malloc(sizeof(dt_develop_blend_params_t));

    memcpy(new->params, old->params, params_size);
    memcpy(new->blend_params, old->blend_params, sizeof(dt_develop_blend_params_t));

    if(old->forms) new->forms = dt_masks_dup_forms_deep(old->forms, NULL);

    result = g_list_append(result, new);

    h = g_list_next(h);
  }
  return result;
}

#undef DT_IOP_ORDER_INFO
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
