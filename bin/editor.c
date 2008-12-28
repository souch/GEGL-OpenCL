/* This file is part of GEGL editor -- a gtk frontend for GEGL
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2003, 2004, 2006 Øyvind Kolås
 */


#define ACTIVE_COLOR cairo_set_source_rgba (cr, 1.0, 0.0, 0.0, 0.5)
#define NORMAL_COLOR cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.5)

#define USE_DYNAMICS 0
#define SUBDIVIDE_DIST 10

#include "config.h"

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <gegl.h>

#include "gegl-bin-gui-types.h"


#include "editor-optype.h"
#include "gegl-node-editor.h"
#include "gegl-options.h"
#include "gegl-store.h"
#include "gegl-tree-editor.h"
#include "gegl-tree-editor-action.h"
#include "gegl-view.h"

#include "editor.h"

#ifdef G_OS_WIN32
#ifndef PATH_MAX
#define PATH_MAX _MAX_PATH
#endif
#define realpath(a,b) _fullpath(b,a,_MAX_PATH)
#endif

#define  KEY_ZOOM_FACTOR  2.0

#include "gegl-path.h"

typedef enum 
{
  STATE_MOVE = 0,
  STATE_STROKES,
  STATE_PICK,
  STATE_PAN,
  STATE_EDIT_NODES,
  STATE_EDIT_WIDTH,
  STATE_EDIT_OPACITY,
  STATE_FREE_REPLACE,
  STATE_REDO_PART, /* redoes part of a path, starting on
                      the first intersection */
} GuiState;


typedef struct
{
  gchar     *operation;
  gboolean (*expose)  (GtkWidget *widget,
                       GdkEvent  *event,
                       gpointer   user_data);
  gboolean (*press)   (GtkWidget      *widget,
                       GdkEventButton *event,
                       gpointer        data);
  gboolean (*release) (GtkWidget    *widget,
                       GdkEventButton *event,
                       gpointer        data);
  gboolean (*motion)  (GtkWidget     *widget,
                       GdkEventMotion *event,
                       gpointer        data);
  void (*activate) (GtkWidget *widget);
  void (*deactivate) (GtkWidget *widget);
} OperationTool;



typedef struct _Tools Tools;
struct _Tools
{
/* paint core globals */
  GuiState     state; /* 0: modify, 1: add strokes, 2: */
  
  GeglNode *node;
  GeglPath *path;
  gint      selected_no;
  gint      drag_no; /* -1 */
  gint      drag_sub;
  gdouble   prevx;
  gdouble   prevy;
  guint32   prevtime;

  gboolean  in_drag;
  /* the pie menu code is written to handle only one pie menu
   * at a time for now
   */
  gboolean  menu_active;

  gdouble   menux;
  gdouble   menuy;

  GeglPath *width_path;

  gint      menu_segments;
  gint      menu_segment_active;
  gchar     menu_segment_label[10][10];
  GCallback menu_segment_callback[10];
  gpointer  menu_segment_userdata[10];
};

Tools  tools;

static void
menu_clear (void)
{
  tools.menu_segment_active = -1;
  tools.menu_segments=0;
}

static void
menu_add (const gchar *label, GCallback callback, gpointer userdata)
{
  strcpy (tools.menu_segment_label[tools.menu_segments], label);
  
  tools.menu_segment_callback[tools.menu_segments]=callback;
  tools.menu_segment_userdata[tools.menu_segments]=userdata;
  tools.menu_segments++;
  g_assert (tools.menu_segments < 10);
}

static gint
do_command (const gchar *command);

Editor editor;

static gchar *blank_composition =
    "<gegl>"
        "<gegl:color value='white'/>"
    "</gegl>";

static void gegl_editor_update_title (void);
static gboolean
cb_window_delete_event (GtkWidget *widget, GdkEvent *event, gpointer data);


static gboolean
gui_keybinding (GdkEventKey *event);


static gboolean
cb_window_keybinding (GtkWidget *widget, GdkEventKey *event, gpointer data)
{
  switch (event->keyval)
    {
      case GDK_l:
      if(event->state & (GDK_CONTROL_MASK &
                         gtk_accelerator_get_default_mod_mask()))
        {
          if (editor.search_entry)
            gtk_widget_grab_focus (editor.search_entry);
          return TRUE;
        }
      break;
      default:
        return gui_keybinding (event);
        break;
    }
  return FALSE;
}

static void gegl_node_get_translation (GeglNode *node,
                                       gdouble  *x,
                                       gdouble  *y)
{
  GeglNode **consumers=NULL;

  if (x)
    *x = 0;
  if (y)
    *y = 0;

  while (node)
    {
      if (gegl_node_get_consumers (node, "output", &consumers, NULL))
        {
          const gchar *opname;
          node = *consumers;
          g_free (consumers);
          opname = gegl_node_get_operation (node);
          if (g_str_equal (opname, "gegl:translate") ||
              g_str_equal (opname, "gegl:translate"))
            {
              gdouble tx, ty;
              gegl_node_get (node, "x", &tx, "y", &ty, NULL);

              if (x)
                *x += tx;
              if (y)
                *y += ty;
            }
        }
      else
        node = NULL;
    }

}

static void foreach_cairo (const GeglPathItem *knot,
                           gpointer            cr)
{
  switch (knot->type)
    {
      case 'M':
        cairo_move_to (cr, knot->point[0].x, knot->point[0].y);
        break;
      case 'L':
        cairo_line_to (cr, knot->point[0].x, knot->point[0].y);
        break;
      case 'C':
        cairo_curve_to (cr, knot->point[0].x, knot->point[0].y,
                            knot->point[1].x, knot->point[1].y,
                            knot->point[2].x, knot->point[2].y);
        break;
      case 'z':
        cairo_close_path (cr);
        break;
      default:
        g_print ("%s uh?:%c\n", G_STRLOC, knot->type);
    }
}

static void gegl_path_cairo_play (GeglPath *vector,
                                  cairo_t  *cr)
{
  gegl_path_foreach_flat (vector, foreach_cairo, cr);
}

static void get_loc (const GeglPathItem *knot, 
                     gdouble        *x,
                     gdouble        *y)
{
  if (knot->type == 'C')
    {
      *x = knot->point[2].x;
      *y = knot->point[2].y;
    }
  else
    {
      *x = knot->point[0].x;
      *y = knot->point[0].y;
    }
}

static void select_node (GeglNode *node)
{
  GtkTreeSelection *selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (tree_editor_get_treeview (editor.tree_editor)));
  GtkTreeIter iter;
  iter.user_data = node;
  gtk_tree_selection_select_iter (selection, &iter);
  tree_editor_set_active (editor.tree_editor, node);
}


static gint add_path (gint argc, gchar **argv)
{
      GeglNode *stroke;
      GeglColor *color = gegl_color_new ("black");

      /* if our parent is an over op, insert our own over op before
       * that over op
       */
      GeglNode *self = tools.node;
      GeglNode *parent;

      if (!self)
        {
          g_object_get (editor.view, "node", &self, NULL);
          self = gegl_node_get_output_proxy (self, "output");
          self = gegl_node_get_producer (self, "input", NULL);
          self = gegl_next_sibling (self);
        }
      parent = gegl_parent (self);

      if (parent && g_str_equal (gegl_node_get_operation (parent), "gegl:over"))
        {
          select_node (parent);
        }
      else
        {
          select_node (self);
        }

      gegl_add_sibling ("gegl:over");
      stroke = gegl_add_child ("gegl:path");

      {
        GeglColor *color2;
        gdouble    linewidth;
        gfloat r,g,b,a;
    
        if (self && g_str_equal (gegl_node_get_operation (self), "gegl:path"))
          {
            gegl_node_get (self, "stroke", &color2, "stroke-width", &linewidth, NULL);
            gegl_color_get_rgba (color2, &r, &g, &b, &a);
            gegl_color_set_rgba (color, r,g,b,a);
          }
        else
          {
            linewidth = 10;
          }

        gegl_node_set (stroke, "d", tools.path=gegl_path_new (), "stroke", color, "stroke-width", linewidth, NULL);
        tools.node = stroke;
        tools.selected_no = 0;
        tools.drag_no = -1;  /* to start dragging at the end? of the path,
                                this is needed to make it start node
                                creation on first event at least */
        tree_editor_set_active (editor.tree_editor, stroke);
      }
    return 0;
}

static gint insert_node (gint argc, gchar **argv)
{
  GeglPathItem knot = *gegl_path_get_node (tools.path, tools.selected_no);
  knot.point[0].x += 10;
  gegl_path_insert_node (tools.path, tools.selected_no, &knot);
  tools.selected_no ++;
  return 0;
}

static gboolean spiro_is_closed (GeglPath *path)
{
  const GeglPathItem *knot;
 
  if (!path)
    return FALSE;
  knot = gegl_path_get_node (tools.path, -1);
  if (!knot)
    return FALSE;
  if (knot->type == 'z')
    {
      return TRUE;
    }
  return FALSE;
}


static gint spiro_open (gint argc, gchar **argv)
{
  GeglPathItem knot = *gegl_path_get_node (tools.path, -1);
  if (knot.type == 'z')
    {
      gegl_path_remove_node (tools.path, -1);
      g_print ("opened path\n");
      return 0;
    }
  g_print ("already open\n");

  return 0;
}

static gint spiro_close (gint argc, gchar **argv)
{
  GeglPathItem knot = *gegl_path_get_node (tools.path, -1);
  if (knot.type == 'z')
    {
      g_print ("already closed\n");
      return -1;
    }
  gegl_path_append (tools.path, 'z');
  g_print ("closed spiro\n");

  {
    /*GeglColor *color= gegl_color_new ("green");
    gegl_node_set (tools.node, "fill", color, "linewidth", 0.0, NULL);
    g_object_unref (color);*/
  }

  return 0;
}

static gint insert_node_before (gint argc, gchar **argv)
{
  GeglPathItem knot = *gegl_path_get_node (tools.path, tools.selected_no);
  g_assert (argv[1] && argv[2]);

  if (tools.selected_no == 0)
    {
      gegl_path_insert_node (tools.path, 0, &knot);
      knot.point[0].x = atof (argv[1]);
      knot.point[0].y = atof (argv[2]);
      gegl_path_replace_node (tools.path, 0, &knot);
    }
  else
    {
      knot.point[0].x = atof (argv[1]);
      knot.point[0].y = atof (argv[2]);
      gegl_path_insert_node (tools.path, tools.selected_no-1, &knot);
    }
  return 0;
}

static gint override_node_after = -1;
static gint insert_node_after (gint argc, gchar **argv)
{
  GeglPathItem knot;
 
  if (tools.selected_no <0)
    {
      override_node_after = -1;
      return -1;
    }
  if (override_node_after != -1)
    knot = *gegl_path_get_node (tools.path, override_node_after);
  else
    knot = *gegl_path_get_node (tools.path, tools.selected_no);
  g_assert (argv[1] && argv[2]);
  knot.point[0].x = atof (argv[1]);
  knot.point[0].y = atof (argv[2]);
  gegl_path_insert_node (tools.path, tools.selected_no, &knot);
  tools.selected_no ++;
  override_node_after = -1;
  return 0;
}

static gint remove_node (gint argc, gchar **argv)
{
  gegl_path_remove_node (tools.path, tools.selected_no);
  if (tools.selected_no>0)
    tools.selected_no --;
  else
    tools.selected_no = 0;
  return 0;
}

static gint clear_path (gint argc, gchar **argv)
{
  gegl_path_clear (tools.path);
  return 0;
}

static gint spiro_mode (gint argc, gchar **argv)
{
  GeglPathItem knot = *gegl_path_get_node (tools.path, tools.selected_no);
  knot.type = argv[1][0];
  g_print ("setting %c\n", knot.type);
  gegl_path_replace_node (tools.path, tools.selected_no, &knot);
  return 0;
}

static gint spiro_mode_change (gint argc, gchar **argv)
{
        GeglPathItem knot = *gegl_path_get_node (tools.path, tools.selected_no);
          switch (knot.type)
            {
              case 'v':
                knot.type = 'o';
                break;
              case 'o':
                knot.type = 'O';
                break;
              case 'O':
                knot.type = '[';
                break;
              case '[':
                knot.type = ']';
                break;
              case ']':
                knot.type = 'v';
                break;
            }
          g_print ("setting %c\n", knot.type);
          gegl_path_replace_node (tools.path, tools.selected_no, &knot);
 
  return 0;
}


static void move_rel (GeglNode *node,
                      gdouble   relx,
                      gdouble   rely)
{
  GeglNode *shift;
 
  for (shift = node; shift && !g_str_equal (gegl_node_get_operation (shift), "gegl:translate");shift=gegl_previous_sibling (shift))
    {
    }

  if (!shift)
    {
      shift = gegl_add_sibling ("gegl:translate");
        {
          select_node (node);
        }
    }
    {
      gdouble x, y;
      gegl_node_get (shift, "x", &x, "y", &y, NULL);
      x+=relx;
      y+=rely;
      gegl_node_set (shift, "x", x, "y", y, NULL);
    }
}


#define shapeaction(x,y,factor,color,label) \
      cairo_new_path (cr);\
      cairo_arc (cr, x, y, (ACTIVE_ARC * factor)/scale, 0.0, 3.1415*2);

#define drawaction(x,y,factor,color,label) {cairo_text_extents_t text_extents;\
      cairo_new_path (cr);\
      cairo_arc (cr, x, y, (ACTIVE_ARC * factor+1)/scale, 0.0, 3.1415*2); \
            cairo_set_source_rgba (cr, 1,1,1, 0.4);\
      cairo_fill (cr);\
      cairo_new_path (cr);\
      cairo_arc (cr, x, y, (ACTIVE_ARC * factor)/scale, 0.0, 3.1415*2); \
      cairo_set_source_rgba (cr, 0,0,0,0.6);\
      cairo_fill_preserve (cr);\
      if (color>0.01) { cairo_set_source_rgba (cr, 1,0.3,0.3,color);cairo_fill(cr);}\
            cairo_select_font_face (cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL,\
                                                CAIRO_FONT_WEIGHT_NORMAL);\
            cairo_set_font_size (cr, 10/scale);\
            cairo_text_extents (cr, label, &text_extents);\
            cairo_move_to (cr, x, y);\
            cairo_rel_move_to (cr, -text_extents.width/2 - text_extents.x_bearing, -text_extents.height/2 - text_extents.y_bearing);\
            cairo_set_source_rgb (cr, 1,1,1);\
            cairo_show_text (cr, label);}



static gboolean
nodes_press_event (GtkWidget      *widget,
                   GdkEventButton *event,
                   gpointer        data)
{
  gint   x, y;
  gdouble scale;
  gdouble tx, ty;
  gdouble ex, ey;
  GeglNode *detected = NULL;

  cairo_t *cr = gdk_cairo_create (widget->window);
  GeglPath *vector;
  const GeglPathItem *knot;
  const GeglPathItem *prev_knot = NULL;
  gint i, n;

  if (!tools.node)
    {
      new_stroke:
      do_command ("add-path");
    }

  g_object_get (G_OBJECT (widget),
                "x", &x,
                "y", &y,
                "scale", &scale,
                NULL);
  gegl_node_get_translation (GEGL_NODE (tools.node), &tx, &ty);

  ex = (event->x + x) / scale - tx;
  ey = (event->y + y) / scale - ty;

  vector = tools.path;


  n= gegl_path_get_n_nodes (vector);

  prev_knot = NULL;
  for (i=0;i<n;i++)
    {
      gdouble x, y;
      knot = gegl_path_get_node (vector, i);

      /* handling of handles on beziers */
      if (knot->type == 'C')
        {

#define ACTIVE_ARC 4.0
#define INACTIVE_ARC 3.0

          if ( i == tools.selected_no + 1)
            {
              x = knot->point[0].x;
              y = knot->point[0].y;
              cairo_new_path (cr);
              cairo_move_to (cr, x, y);
              cairo_arc (cr, x, y, ACTIVE_ARC/scale, 0.0, 3.1415*2);
              if (cairo_in_fill (cr, ex, ey))
                {
                  tools.drag_no = i -1;
                  tools.drag_sub = -1;
                  tools.prevx = ex;
                  tools.prevy = ey;
                }
            }

          x = knot->point[1].x;
          y = knot->point[1].y;
          cairo_move_to (cr, x, y);

          if ( i == tools.selected_no)
            {
              cairo_new_path (cr);
              cairo_arc (cr, x, y, ACTIVE_ARC/scale, 0.0, 3.1415*2);
              if (cairo_in_fill (cr, ex, ey))
                {
                  tools.prevx = ex;
                  tools.prevy = ey;
                  tools.drag_no = i;
                  tools.drag_sub = 1;
                }
            }
          gtk_widget_queue_draw (widget);
      cairo_destroy (cr);
          return TRUE;
        }

      get_loc (knot, &x, &y);
      shapeaction (x, y, 2, 0, ".");
      if (cairo_in_fill (cr, ex, ey))
        {

          if (i==0 && tools.selected_no == n-1)
            {
              do_command ("spiro-close");
              tools.selected_no = tools.drag_no = 0;
            }
          else if (i==n-1 && tools.selected_no == 0)
            {
              do_command ("spiro-close");
              tools.selected_no = tools.drag_no = n-1;
            }
          else tools.selected_no = tools.drag_no = i;

          tools.drag_sub = 0;
          tools.prevx = ex;
          tools.prevy = ey;
          gtk_widget_queue_draw (widget);

          /* dragging a knot */

          cairo_destroy (cr);
          return TRUE;
        }

      if ( i == tools.selected_no)
        {
          gdouble sx, sy; /* satelite */

#define P2(y) ((y)*(y))
#define DIST(x0,y0,x1,y1)  (sqrt(P2(x1-x0)+P2(y1-y0)))

          if (i > 0 &&
              n > 1 &&
              i < n-1 &&
              n > 1 &&
              0)
            {
              const GeglPathItem *knot2;
              gdouble px, py, nx, ny; /* prev, next */
              px = prev_knot->point[0].x;
              py = prev_knot->point[0].y;
              knot2 = gegl_path_get_node (vector, i + 1);

              nx = knot2->point[0].x;
              ny = knot2->point[0].y;

              {
                gdouble len;
                sx = (px-x)/DIST(x,y,px,py) + (nx-x)/DIST(x,y,nx,ny);
                sy = (py-y)/DIST(x,y,px,py) + (ny-y)/DIST(x,y,nx,ny);
                len = sqrt (sx*sx+sy*sy);
                sx /= len;
                sy /= len;
                sx *= -ACTIVE_ARC * 3 / scale;
                sy *= -ACTIVE_ARC * 3 / scale;
              }
            }
          else
            {
              sx = 0;
              sy = -ACTIVE_ARC * 3.5 / scale;
            }


#define CMD(cmd)\
                if (cairo_in_fill (cr, ex, ey))\
                  {\
                    do_command (cmd);\
                    gtk_widget_queue_draw (widget);\
                    return TRUE;\
                  }

          shapeaction (x-sx*1, y-sy*1, 1.5, 0.5, "X");
          CMD("remove-node");

          if ((i==0 || i==n-1) && !spiro_is_closed (vector))
          switch (knot->type)
            {

              case 'v':
                shapeaction (x+sx*1, y+sy*1, 1.5, 0.5, "O");
                CMD("spiro-mode O");
                break;
              case 'o':
              case 'O':
                shapeaction (x+sx*1, y+sy*1, 1.5, 0.5, "v");
                CMD("spiro-mode v");
                break;
              case '[':
                shapeaction (x+sx*1, y+sy*1, 1.5, 0.5, "]");
                CMD("spiro-mode ]");
                shapeaction (x+sx*2, y+sy*2, 1.5, 0.5, "v");
                CMD("spiro-mode v");
                shapeaction (x+sx*3, y+sy*3, 1.5, 0.5, "O");
                CMD("spiro-mode O");
                break;
              case ']':
                shapeaction (x+sx*1, y+sy*1, 1.5, 0.5, "[");
                CMD("spiro-mode [");
                shapeaction (x+sx*2, y+sy*2, 1.5, 0.5, "v");
                CMD("spiro-mode v");
                shapeaction (x+sx*3, y+sy*3, 1.5, 0.5, "O");
                CMD("spiro-mode O");
                break;
              case '*':
                shapeaction (x+sx*1, y+sy*1, 1.5, 0.5, "v");
                CMD("spiro-mode v");
              default:
                break;
            }
          else switch (knot->type)
            {

              case 'v':
                shapeaction (x+sx*1, y+sy*1, 1.5, 0.5, "O");
                CMD("spiro-mode O");
                shapeaction (x+sx*2, y+sy*2, 1.5, 0.5, "[");
                CMD("spiro-mode [");
                shapeaction (x+sx*3, y+sy*3, 1.5, 0.5, "]");
                CMD("spiro-mode ]");
                break;
              case 'o':
              case 'O':
                shapeaction (x+sx*1, y+sy*1, 1.5, 0.5, "v");
                CMD("spiro-mode v");
                shapeaction (x+sx*2, y+sy*2, 1.5, 0.5, "[");
                CMD("spiro-mode [");
                shapeaction (x+sx*3, y+sy*3, 1.5, 0.5, "]");
                CMD("spiro-mode ]");
                break;
              case '[':
                shapeaction (x+sx*1, y+sy*1, 1.5, 0.5, "]");
                CMD("spiro-mode ]");
                shapeaction (x+sx*2, y+sy*2, 1.5, 0.5, "v");
                CMD("spiro-mode v");
                shapeaction (x+sx*3, y+sy*3, 1.5, 0.5, "O");
                CMD("spiro-mode O");
                break;
              case ']':
                shapeaction (x+sx*1, y+sy*1, 1.5, 0.5, "[");
                CMD("spiro-mode [");
                shapeaction (x+sx*2, y+sy*2, 1.5, 0.5, "v");
                CMD("spiro-mode v");
                shapeaction (x+sx*3, y+sy*3, 1.5, 0.5, "O");
                CMD("spiro-mode O");
                break;
              case '*':
                shapeaction (x+sx*1, y+sy*1, 1.5, 0.5, "v");
                CMD("spiro-mode v");
              default:
                break;
            }
        }
      prev_knot = knot;
    }

        {
          GeglNode *node;
          g_object_get (editor.view, "node", &node, NULL);
          detected = gegl_node_detect (node, ex + tx, ey + tx);
          g_object_unref (node);
        }

      {
        gdouble linewidth;
        cairo_new_path (cr);
        gegl_path_cairo_play (vector, cr);
        gegl_node_get (tools.node, "linewidth", &linewidth, NULL);
        cairo_set_line_width (cr, (SUBDIVIDE_DIST*2)/scale);  /* 5px wide active zone */


      if (cairo_in_stroke (cr, ex, ey)) 
        {
          /* subdivide segment */

          gdouble pos;
          gint node_before;
          pos = gegl_path_closest_point (vector, ex,ey, &ex, &ey, &node_before);

            {
              gchar buf[256];
              override_node_after = tools.selected_no; /* evil hack */
              tools.selected_no = node_before;
              sprintf (buf, "insert-node-after %f %f", ex, ey);
              g_print ("%s %i\n", buf, node_before);
              do_command (buf);

              tools.drag_no = tools.selected_no = node_before + 1;
              tools.drag_sub = 0;
              tools.prevx = ex;
              tools.prevy = ey;
              gtk_widget_queue_draw (widget);
              return TRUE;
            }
        }
      else if (spiro_is_closed (tools.path) &&
               cairo_in_fill (cr, ex, ey) /*&&
               (detected && (detected != tools.node))*/)
        {
          tools.prevx += tx;
          tools.prevy += ty;
          tools.in_drag = TRUE;
          cairo_destroy (cr);
          gtk_widget_queue_draw (widget);
          return TRUE;
        }

          /* deal with clicks outside path */

          if (detected)
            {
              if (g_str_equal (gegl_node_get_operation (detected), "gegl:path"))
                {
                  select_node (detected);
                  goto done;
                }
            }


          if ((n-1 == tools.selected_no) && tools.drag_no < 0 && !spiro_is_closed (vector))
            {
              if (prev_knot)
                {
                      GeglPathItem knot = {prev_knot->type, {{ex, ey}}};
                      gegl_path_insert_node (vector, -1, &knot);
                      tools.selected_no = tools.drag_no = n;
                      tools.drag_sub = 0;
                      tools.prevx = ex;
                      tools.prevy = ey;
                }
              else
                {
                      GeglPathItem knot = {'V', {{ex, ey}}};
                      gegl_path_insert_node (vector, -1, &knot);
                      tools.selected_no = tools.drag_no = n;
                      tools.drag_sub = 0;
                      tools.prevx = ex;
                      tools.prevy = ey;
                }

              cairo_destroy (cr);
              gtk_widget_queue_draw (widget);
              return FALSE;
            }
          else if (tools.selected_no == 0 && !spiro_is_closed (vector))
            {
              g_print ("start add\n");
              {
                gchar buf[256];

                if (!prev_knot)
                  {
                    GeglPathItem knot = {'v', {{ex, ey}}};
                    gegl_path_insert_node (vector, -1, &knot);
                  }
                else
                  {
                    sprintf (buf, "insert-node-before %f %f", ex, ey);
                    do_command (buf);
                  }

                tools.selected_no = tools.drag_no = 0;
                tools.drag_sub = 0;
                tools.prevx = ex;
                tools.prevy = ey;
                gtk_widget_queue_draw (widget);
              }
            }
            else
              {
                goto new_stroke;
              }
      }

done:
  cairo_destroy (cr);
  gtk_widget_queue_draw (widget);

  return FALSE;
}


static gboolean
nodes_release_event (GtkWidget      *widget,
                      GdkEventButton *event,
                      gpointer        data)
{
  tools.drag_no = -1;
  tools.in_drag = FALSE;
  gtk_widget_queue_draw (widget);
  return FALSE;
}

static gboolean
nodes_motion_notify_event (GtkWidget      *widget,
                           GdkEventMotion *event,
                           gpointer        data)
{
    {
      gint   x, y;
      gint n;
      gdouble scale;
      gdouble tx, ty;
      gdouble ex, ey;
      gdouble rx, ry;

      GeglPath *vector;
      GeglPathItem  new_knot;

      g_object_get (G_OBJECT (widget),
                    "x", &x,
                    "y", &y,
                    "scale", &scale,
                    NULL);

      if (!tools.node || !tools.path)
        return FALSE;




      gegl_node_get_translation (tools.node, &tx, &ty);
      ex = (event->x + x) / scale;
      ey = (event->y + y) / scale;

      if (!tools.in_drag)
        {
          ex -= tx;
          ey -= ty;
        }

      rx = tools.prevx - ex;
      ry = tools.prevy - ey;
      tools.prevx = ex;
      tools.prevy = ey;

      if (tools.in_drag)
        {
          move_rel (tools.node, -rx, -ry);
          gtk_widget_queue_draw (widget);
          return TRUE;
        }


      vector = tools.path;

      if (tools.drag_no != -1)
      {
      if (tools.drag_sub == 0)
        {
          new_knot = *gegl_path_get_node (vector, tools.drag_no);
          if (new_knot.type == 'C')
            {
              new_knot.point[1].x -= rx;
              new_knot.point[1].y -= ry;
              new_knot.point[2].x -= rx;
              new_knot.point[2].y -= ry;
              gegl_path_replace_node (vector, tools.drag_no, &new_knot);
              new_knot = *gegl_path_get_node (vector, tools.drag_no + 1);
              new_knot.point[0].x -= rx;
              new_knot.point[0].y -= ry;
              gegl_path_replace_node (vector, tools.drag_no + 1, &new_knot);
            }
          else
            {
              new_knot.point[0].x -= rx;
              new_knot.point[0].y -= ry;
              gegl_path_replace_node (vector, tools.drag_no, &new_knot);
            }
          gtk_widget_queue_draw (widget);
        }
      else if (tools.drag_sub == 1)
        {
          new_knot = *gegl_path_get_node (vector, tools.drag_no);
          new_knot.point[1].x -= rx;
          new_knot.point[1].y -= ry;
          gegl_path_replace_node (vector, tools.drag_no, &new_knot);
          gtk_widget_queue_draw (widget);
        }
      else if (tools.drag_sub == -1)
        {
          new_knot = *gegl_path_get_node (vector, tools.drag_no + 1);
          new_knot.point[0].x -= rx;
          new_knot.point[0].y -= ry;
          gegl_path_replace_node (vector, tools.drag_no + 1, &new_knot);
          gtk_widget_queue_draw (widget);
        }
      }

      /* make the closest the selected */

      n = gegl_path_get_n_nodes (vector);
      if ((tools.selected_no != 0 &&
          tools.selected_no != n -1)
          || spiro_is_closed (vector))
      {
        gint i;
        gint closest=0;
        gdouble bestdist = 100000;
        for (i=0;i<n;i++)
          {
            const GeglPathItem *node;
            gdouble dist;
            node = gegl_path_get_node (vector, i);
            dist = DIST(ex,ey,node->point[0].x,
                              node->point[0].y);
            if (dist < bestdist)
              {
                bestdist = dist;
                closest = i;
              }
            
          }
        tools.selected_no = closest;
      }

      gtk_widget_queue_draw (widget);
      return TRUE;
    }
  return FALSE;
}



static gboolean nodes_expose (GtkWidget *widget,
                             GdkEvent  *event,
                             gpointer   user_data)
{

  gint   x, y;
  gdouble scale;
  gdouble tx, ty;

  cairo_t *cr = gdk_cairo_create (widget->window);
  GeglPath *vector;
  const GeglPathItem *knot;
  const GeglPathItem *prev_knot = NULL;
  gint i;
  gint n;


  g_object_get (G_OBJECT (widget),
                "x", &x,
                "y", &y,
                "scale", &scale,
                NULL);

  cairo_translate (cr, -x, -y);
  cairo_scale (cr, scale, scale);

  if (!tools.node || !tools.path)
    {
      return FALSE;
    }

  gegl_node_get_translation (GEGL_NODE (tools.node), &tx, &ty);

  cairo_translate (cr, tx, ty);


  vector = tools.path;

  cairo_new_path (cr);
  gegl_path_cairo_play (vector, cr);

  n= gegl_path_get_n_nodes (vector);
  prev_knot = NULL;
  for (i=0;i<n;i++)
    {
      gdouble x, y;
      knot = gegl_path_get_node (vector, i);
      if (knot->type == 'C')
        {
          get_loc (prev_knot, &x, &y);
          cairo_move_to (cr, x, y);
          cairo_line_to (cr, knot->point[0].x, knot->point[0].y);
          cairo_move_to (cr, knot->point[1].x, knot->point[1].y);
          cairo_line_to (cr, knot->point[2].x, knot->point[2].y);
        }
      prev_knot = knot;
    }

  cairo_set_line_width (cr, 4.0/scale);
  cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.6);
  cairo_stroke_preserve (cr);
  cairo_set_line_width (cr, 2.4/scale);
  cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.8);
  cairo_stroke (cr);

  n= gegl_path_get_n_nodes (vector);
  prev_knot = NULL;
  if (tools.drag_no==-1)
    {

     {
       gdouble pos;
       gdouble px, py;
       gint node_before;
       pos = gegl_path_closest_point (vector, tools.prevx,tools.prevy, &px, &py, &node_before);

       if (DIST(px,py,tools.prevx,tools.prevy) < SUBDIVIDE_DIST/scale)
         {
            drawaction (px, py, 1.5, 0.5, "+");
         }
       else if ((tools.selected_no == 0 ||
                 tools.selected_no == n-1) &&
                 !spiro_is_closed (vector))
         {
            cairo_move_to (cr, tools.prevx, tools.prevy);
            drawaction (tools.prevx, tools.prevy, 1.5, 0.5, ".");
         }
     }
  for (i=0;i<n;i++)
    {
      gdouble x, y;
      knot = gegl_path_get_node (vector, i);
      if (!knot)
        g_error ("EEEK!");
         
        
      if (knot->type == 'C')
        {
          x = knot->point[0].x;
          y = knot->point[0].y;
          cairo_move_to (cr, x, y);

#define ACTIVE_COLOR cairo_set_source_rgba (cr, 1.0, 0.0, 0.0, 0.5)
#define NORMAL_COLOR cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.5)

          if ( i == tools.selected_no + 1)
            {
              ACTIVE_COLOR;
              cairo_arc (cr, x, y, ACTIVE_ARC/scale, 0.0, 3.1415*2);
            }
          else
            {
              NORMAL_COLOR;
              cairo_arc (cr, x, y, INACTIVE_ARC/scale, 0.0, 3.1415*2);
            }
          cairo_fill (cr);

          x = knot->point[1].x;
          y = knot->point[1].y;
          cairo_move_to (cr, x, y);

          if ( i == tools.selected_no)
            {
              ACTIVE_COLOR;
              cairo_arc (cr, x, y, ACTIVE_ARC/scale, 0.0, 3.1415*2);
            }
          else
            {
              NORMAL_COLOR;
              cairo_arc (cr, x, y, INACTIVE_ARC/scale, 0.0, 3.1415*2);
            }
          cairo_fill (cr);
        }

      get_loc (knot, &x, &y);
      cairo_move_to (cr, x, y);
      /*cairo_arc (cr, x, y, ACTIVE_ARC/scale, 0.0, 3.1415*2);*/

      if (knot->type == 'z')
        {
        }
      else if (i == tools.selected_no)
        {
          gchar buf[2]=".";
          buf[0] = knot->type;
          drawaction(x,y, 2, 1.0, buf);
        }
      else /*if (abs (i - tools.selected_no) < 2)*/
        {
          gchar buf[2]=".";
          buf[0] = knot->type;
          drawaction(x,y, 2, 0.0, buf);
        }

      if ( i == tools.selected_no)
        ACTIVE_COLOR;
      else
        NORMAL_COLOR;
      cairo_fill (cr);


      if ( i == tools.selected_no)
        {
          gdouble sx, sy; /* satelite */


          if (i > 0 &&
              n > 1 &&
              i < n-1 &&
              n > 1 &&
              0)
            {
              const GeglPathItem *knot2;
              gdouble px, py, nx, ny; /* prev, next */
              px = prev_knot->point[0].x;
              py = prev_knot->point[0].y;
              knot2 = gegl_path_get_node (vector, i + 1);

              nx = knot2->point[0].x;
              ny = knot2->point[0].y;

              {
                gdouble len;
                sx = (px-x)/DIST(x,y,px,py) + (nx-x)/DIST(x,y,nx,ny);
                sy = (py-y)/DIST(x,y,px,py) + (ny-y)/DIST(x,y,nx,ny);
                len = sqrt (sx*sx+sy*sy);
                sx /= len;
                sy /= len;
                sx *= -ACTIVE_ARC * 3 / scale;
                sy *= -ACTIVE_ARC * 3 / scale;
              }
            }
          else
            {
              sx = 0;
              sy = -ACTIVE_ARC * 3.5 / scale;

            }

        /* don't draw the action items while dragging */
        if (tools.drag_no == -1)
          {
            drawaction (x-sx*1, y-sy*1, 1.5, 0.5, "X");
            if ((i==0 || i==n-1) && !spiro_is_closed (vector))
            switch (knot->type)
              {
                case 'v':
                  drawaction (x+sx*1, y+sy*1, 1.5, 0.5, "O");
                  break;
                case 'o':
                case 'O':
                  drawaction (x+sx*1, y+sy*1, 1.5, 0.5, "v");
                  break;
                case '[':
                  drawaction (x+sx*1, y+sy*1, 1.5, 0.5, "]");
                  drawaction (x+sx*2, y+sy*2, 1.5, 0.5, "v");
                  drawaction (x+sx*3, y+sy*3, 1.5, 0.5, "O");
                  break;
                case ']':
                  drawaction (x+sx*1, y+sy*1, 1.5, 0.5, "[");
                  drawaction (x+sx*2, y+sy*2, 1.5, 0.5, "v");
                  drawaction (x+sx*3, y+sy*3, 1.5, 0.5, "O");
                  break;
                case '*':
                  drawaction (x+sx*1, y+sy*1, 1.5, 0.5, "v");
                  break;
                  break;
                default:
                  break;
              }
            else switch (knot->type)
              {
                case 'v':
                  drawaction (x+sx*1, y+sy*1, 1.5, 0.5, "O");
                  drawaction (x+sx*2, y+sy*2, 1.5, 0.5, "[");
                  drawaction (x+sx*3, y+sy*3, 1.5, 0.5, "]");
                  break;
                case 'o':
                case 'O':
                  drawaction (x+sx*1, y+sy*1, 1.5, 0.5, "v");
                  drawaction (x+sx*2, y+sy*2, 1.5, 0.5, "[");
                  drawaction (x+sx*3, y+sy*3, 1.5, 0.5, "]");
                  break;
                case '[':
                  drawaction (x+sx*1, y+sy*1, 1.5, 0.5, "]");
                  drawaction (x+sx*2, y+sy*2, 1.5, 0.5, "v");
                  drawaction (x+sx*3, y+sy*3, 1.5, 0.5, "O");
                  break;
                case ']':
                  drawaction (x+sx*1, y+sy*1, 1.5, 0.5, "[");
                  drawaction (x+sx*2, y+sy*2, 1.5, 0.5, "v");
                  drawaction (x+sx*3, y+sy*3, 1.5, 0.5, "O");
                  break;
                case '*':
                  drawaction (x+sx*1, y+sy*1, 1.5, 0.5, "v");
                  break;
                  break;
                default:
                  break;
              }
          }
        }
    }
    }



  cairo_destroy (cr);
  return FALSE;
}

static gboolean
width_press_event (GtkWidget      *widget,
                   GdkEventButton *event,
                   gpointer        data)
{
  gint   x, y;
  gdouble scale;
  gdouble tx, ty;
  gdouble ex, ey;

  cairo_t *cr = gdk_cairo_create (widget->window);
  GeglPath *vector;

  g_object_get (G_OBJECT (widget),
                "x", &x,
                "y", &y,
                "scale", &scale,
                NULL);
  gegl_node_get_translation (GEGL_NODE (tools.node), &tx, &ty);

  ex = (event->x + x) / scale - tx;
  ey = (event->y + y) / scale - ty;

  vector = tools.path;

    {
  GeglPath *width_profile;
  gdouble linewidth;
  const GeglPathItem *knot;
  gint i;
  gint n;

  gegl_node_get (tools.node, "linewidth", &linewidth, NULL);

  width_profile = gegl_path_get_parameter_path (vector, "linewidth");

  if (width_profile)
    {
      n= gegl_path_get_n_nodes (width_profile);
      for (i=0;i<n;i++)
        {
          gdouble x, y;
          knot = gegl_path_get_node (width_profile, i);
          if (knot->type == '_')
            {
              gegl_path_calc (vector, knot->point[0].x, &x, &y);
              cairo_new_path (cr);
              cairo_move_to (cr, x, y);
              cairo_arc (cr, x, y, linewidth * knot->point[0].y/2, 0, 2*3.1415);
              if (cairo_in_fill (cr, ex, ey))
                {
                  if (tools.selected_no != i)
                    gtk_widget_queue_draw (widget);
                  tools.selected_no = i;
                  tools.drag_no = i;
                  tools.drag_sub = 0;
                  tools.prevx = ex;
                  tools.prevy = ey;
                }
            }
        }
    }
    }

  cairo_destroy (cr);

  return FALSE;
}


static gboolean
width_release_event (GtkWidget      *widget,
                     GdkEventButton *event,
                     gpointer        data)
{
  tools.drag_no = -1;
  return FALSE;
}

static gboolean
width_motion_notify_event (GtkWidget      *widget,
                           GdkEventMotion *event,
                           gpointer        data)
{
  if (tools.drag_no != -1)
    {
      gint   x, y;
      gdouble scale;
      gdouble tx, ty;
      gdouble ex, ey;
      gdouble rx, ry;
      GeglPath *width_profile;
      gdouble linewidth;
      const GeglPathItem *knot;
      GeglPathItem new_knot;
      gdouble cx, cy;

      GeglPath *vector;

      g_object_get (G_OBJECT (widget),
                    "x", &x,
                    "y", &y,
                    "scale", &scale,
                    NULL);
      gegl_node_get_translation (tools.node, &tx, &ty);

      ex = (event->x + x) / scale - tx;
      ey = (event->y + y) / scale - ty;

      rx = tools.prevx - ex;
      ry = tools.prevy - ey;

      /* get original coordinates of path point */

      vector = tools.path;

      gegl_node_get (tools.node, "linewidth", &linewidth, NULL);

      width_profile = gegl_path_get_parameter_path (vector, "linewidth");

      if (width_profile)
        {
          knot = gegl_path_get_node (width_profile, tools.drag_no);
          if (knot->type == '_')
            {
              gdouble radius;
              new_knot = *knot;
              gegl_path_calc (vector, knot->point[0].x, &cx, &cy);
              radius = sqrt ((ex-cx)*(ex-cx)+((ey-cy)*(ey-cy)));

              new_knot.point[0].y = radius / (linewidth/2);
              if (new_knot.point[0].y > 1.0)
                new_knot.point[0].y = 1.0;
              else if (new_knot.point[0].y < 0.05)
                new_knot.point[0].y = 0.05;

              gegl_path_replace_node (width_profile, tools.drag_no, &new_knot);
            }
        }
     }

  return FALSE;
}

static gboolean cairo_expose_width (GtkWidget *widget,
                                    GdkEvent  *event,
                                    gpointer   user_data)
{

  gint   x, y;
  gdouble scale;
  gdouble tx, ty;
  gdouble linewidth;

  cairo_t *cr = gdk_cairo_create (widget->window);
  GeglPath *vector;
  GeglPath *width_profile;
  const GeglPathItem *knot;
  gint i;
  gint n;

  g_object_get (G_OBJECT (widget),
                "x", &x,
                "y", &y,
                "scale", &scale,
                NULL);
  gegl_node_get_translation (GEGL_NODE (tools.node), &tx, &ty);

  cairo_translate (cr, -x, -y);
  cairo_scale (cr, scale, scale);
  cairo_translate (cr, tx, ty);

  vector = tools.path;
  gegl_node_get (tools.node, "linewidth", &linewidth, NULL);


  width_profile = gegl_path_get_parameter_path (vector, "linewidth");
  if (width_profile == NULL)
    {
      width_profile = gegl_path_add_parameter_path (vector, "linewidth");
      gegl_path_append (width_profile, '_', 0.0, 0.2);
      gegl_path_append (width_profile, '_', 10.0, 0.2);
      gegl_path_append (width_profile, '_', 45.0, 0.7);
      gegl_path_append (width_profile, '_', 80.0, 0.4);
      gegl_path_append (width_profile, '_', 90.0, 1.0);
      gegl_path_append (width_profile, '_', 120.0, 1.0);
      gegl_path_append (width_profile, '_', 250.0, 1.0);
      gegl_path_append (width_profile, '_', 270.0, 0.5);
      gegl_path_append (width_profile, '_', 275.0, 0.5);
      gegl_path_append (width_profile, '_', 280.0, 1.0);
    }

  cairo_new_path (cr);
  gegl_path_cairo_play (vector, cr);

  cairo_set_line_width (cr, 3.5/scale);
  cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 1.0);
  cairo_stroke_preserve (cr);
  cairo_set_line_width (cr, 2.0/scale);
  cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.5);
  cairo_stroke (cr);

    {
      n= gegl_path_get_n_nodes (width_profile);
      for (i=0;i<n;i++)
        {
          gdouble x, y;
          knot = gegl_path_get_node (width_profile, i);
          if (knot->type == '_')
            {
              gegl_path_calc (vector, knot->point[0].x, &x, &y);
              cairo_new_path (cr);
              cairo_move_to (cr, x, y);
              cairo_arc (cr, x, y, linewidth * knot->point[0].y/2, 0, 2*3.1415);

              if ( i == tools.selected_no)
                {
                  ACTIVE_COLOR;
                }
              else
                {
                  NORMAL_COLOR;
                }

              cairo_fill (cr);
            }
        }
    }


  cairo_destroy (cr);
  return FALSE;
}

static gboolean
gui_keybinding (GdkEventKey *event)
{
  switch (tools.state)
    {
      case STATE_PICK:
        return FALSE;
      case STATE_EDIT_NODES:
        switch (event->keyval)
          {
            case GDK_i: do_command ("insert-node"); return TRUE;
            case GDK_s: do_command ("spiro-mode-change"); return TRUE;

            case GDK_m: do_command ("spiro-open"); return TRUE;

            case GDK_x: do_command ("remove-node"); return TRUE;
            case GDK_o: do_command ("spiro-mode O"); return TRUE;
            case GDK_v: do_command ("spiro-mode v"); return TRUE;
            default:
              return FALSE;
          }
      default:
        break;
    }
  return FALSE;
}

static void path_slice (cairo_t *cr,
                        gint x, gint y,
                        gint inner, gint outher,
                        gdouble adjustment,
                        gint segment, gint n)
{
  gdouble margin = 0.05 * (3.1415*2)/n;

  cairo_new_path (cr);
  cairo_arc (cr, x, y, outher, segment*(3.1415*2)/n + margin, (segment+1)*(3.1415*2)/n -margin);
  margin *= adjustment;
  cairo_arc_negative (cr, x, y, inner, (segment+1)*(3.1415*2)/n - margin, (segment)*(3.1415*2)/n + margin);
  cairo_close_path (cr);
}

void gegl_remove_item (GeglNode *node);
void gegl_move_item_up (GeglNode *node);
void gegl_move_item_down (GeglNode *node);


static gint raise_item (gint argc, char **argv)
{
  GeglNode *self = tree_editor_get_active (editor.tree_editor);
  GeglNode *parent = gegl_parent (self);

  if (g_str_equal (gegl_node_get_operation (parent), "gegl:over"))
    {
      gegl_move_item_up (parent);
      tree_editor_set_active (editor.tree_editor, self);
    }
  return 0;
}

static gint lower (gint argc, char **argv)
{
  GeglNode *self = tree_editor_get_active (editor.tree_editor);
  GeglNode *parent = gegl_parent (self);

  if (g_str_equal (gegl_node_get_operation (parent), "gegl:over"))
    {
      gegl_move_item_down (parent);
      tree_editor_set_active (editor.tree_editor, self);
    }
  return 0;
}

static gint remove_item (gint argc, char **argv)
{
  GeglNode *self = tree_editor_get_active (editor.tree_editor);
  GeglNode *parent = gegl_parent (self);

  if (g_str_equal (gegl_node_get_operation (parent), "gegl:over"))
    {
      gegl_remove_item (parent);
      g_print ("removed item\n");
    }
  return 0;
}

static gdouble sumdist (gint n,
                        gdouble *samples_x,
                        gdouble *samples_y,
                        gdouble *samples_x2,
                        gdouble *samples_y2)
{
  gint i;
  gdouble squaresum =0;
  for (i=0;i<n;i++)
    {
      gdouble dx = samples_x2[i] - samples_x[i];
      gdouble dy = samples_y2[i] - samples_y[i];
      squaresum += (dx*dx + dx*dy);
    }
  return sqrt (squaresum);
}

static gint path_smoothen (gint argc, char **argv)
{
  GeglPath *path = tools.path;
  gdouble length = gegl_path_get_length (path);
  gint     i, n;
  gdouble  *samples_x;
  gdouble  *samples_y;

  gdouble  *samples_x2;
  gdouble  *samples_y2;

  gboolean *skiplist;
  gint iter = 0;

  n = length / 5;

  samples_x = g_malloc (sizeof (gdouble)* n);
  samples_y = g_malloc (sizeof (gdouble)* n);
  samples_x2 = g_malloc (sizeof (gdouble)* n);
  samples_y2 = g_malloc (sizeof (gdouble)* n);
  skiplist = g_malloc0 (sizeof (gboolean)* n);

  gegl_path_calc_values (path, n, samples_x, samples_y);
  gegl_path_freeze (path);

  for (iter=1; iter < n * 0.9 ;iter++)
    {
      gint tryno = -1;
      tryno = g_random_int_range (0, n-1);
      while (tryno <0 ||
             skiplist[tryno])
        tryno++;
      if (tryno >= n)
        continue;

      gegl_path_clear (path);
      for (i=0;i<n;i++)
        {
          if (i != tryno && skiplist[i] == FALSE)
            gegl_path_append (path, 'O', samples_x[i], samples_y[i]);
        }
      gegl_path_calc_values (path, n, samples_x2, samples_y2);
      if (sumdist (n, samples_x, samples_y, samples_x2, samples_y2) < 25)
        {
          skiplist[tryno]=TRUE;
          g_print ("(%2.1f)x", (iter*100.0/n));
        }
      else
          g_print ("[%2.1f]", (iter*100.0/n));
    }

  g_free (samples_x);
  g_free (samples_y);
  g_free (samples_x2);
  g_free (samples_y2);
  gegl_path_thaw (path);

  return 0;
}

static gint set_state (gint argc, char **argv)
{
  if (argv[1]==NULL)
    return 0;
  tools.drag_no = -1;
  if (g_str_equal (argv[1], "pick"))
    {
      tools.state = STATE_PICK;
      gtk_widget_queue_draw (editor.view);
    }
  else if (g_str_equal (argv[1], "move"))
    {
      tools.state = STATE_MOVE;
      gtk_widget_queue_draw (editor.view);
    }
  else if (g_str_equal (argv[1], "strokes"))
    {
      tools.state = STATE_STROKES;
      gtk_widget_queue_draw (editor.view);
    }
  else if (g_str_equal (argv[1], "edit-nodes"))
    {
      tools.state = STATE_EDIT_NODES;
      gtk_widget_queue_draw (editor.view);
    }
  else if (g_str_equal (argv[1], "edit-width"))
    {
      tools.state = STATE_EDIT_WIDTH;
      gtk_widget_queue_draw (editor.view);
    }
  else
    {
      g_warning ("doesn't handle state change to %s\n", argv[1]);
      return -1;
    }
  return 0;
}

static gboolean cairo_gui_expose (GtkWidget *widget,
                                  GdkEvent  *event,
                                  gpointer   user_data)
{
  switch (tools.state)
    {
      case STATE_MOVE:
      case STATE_PICK:
        {
          gint x, y;
          gdouble scale;
          gdouble tx, ty;
          cairo_t *cr = gdk_cairo_create (widget->window);
          cairo_save (cr);
          g_object_get (G_OBJECT (widget),
                        "x", &x,
                        "y", &y,
                        "scale", &scale,
                        NULL);

          if (tools.node)
            {
              GeglRectangle bounds;
          gegl_node_get_translation (GEGL_NODE (tools.node), &tx, &ty);

          cairo_translate (cr, -x, -y);
          if(1)cairo_scale (cr, scale, scale);


          cairo_translate (cr, tx, ty);
              ACTIVE_COLOR;
              bounds = gegl_node_get_bounding_box (tools.node);
              cairo_rectangle (cr, bounds.x, bounds.y, bounds.width, bounds.height);
              cairo_clip_preserve (cr);

              cairo_set_line_width (cr, 4.0 / scale);
              cairo_stroke (cr);
            }
          cairo_restore (cr);

          cairo_destroy (cr);
        }
        break;
      case STATE_EDIT_NODES:
        nodes_expose (widget, event, user_data);
        break;
      case STATE_EDIT_WIDTH:
        cairo_expose_width (widget, event, user_data);
        break;
      case STATE_STROKES:
#if 0
        {
          gint x, y;
          gdouble scale;
          cairo_t *cr = gdk_cairo_create (widget->window);
          g_object_get (G_OBJECT (widget),
                        "x", &x,
                        "y", &y,
                        "scale", &scale,
                        NULL);

          cairo_translate (cr, -x, -y);
          cairo_set_source_rgba (cr, 1.0, 0.0, 1.0, 1.0);

          cairo_move_to (cr, 10, 10);
          cairo_rectangle (cr, 10, 10, 100, 100);
          cairo_fill (cr);
          cairo_destroy (cr);
        }
#endif
        break;
      case STATE_EDIT_OPACITY:
      case STATE_FREE_REPLACE:
      default:
        g_warning ("not handling expose of state %i\n", tools.state);
        break;
    }

  if (tools.menu_active)
    {
      gint x, y;
      gdouble scale;
      cairo_t *cr = gdk_cairo_create (widget->window);
      g_object_get (G_OBJECT (widget),
                    "x", &x,
                    "y", &y,
                    "scale", &scale,
                    NULL);

      cairo_translate (cr, -x, -y);

      cairo_set_source_rgba (cr, 1.0, 0.0, 1.0, 1.0);

      x = tools.menux;
      y = tools.menuy;

      {
        gint segment;
        gint segments = tools.menu_segments;

        gint inner = 35;
        gint outher = 100;
        gint middle = (inner+outher)/2;
        gdouble adjustment = 3.0; /* adjustments to make the margin produce
                                     a straight line from the centre
                                     (set by trial and error) */


        for (segment=0;segment<segments;segment++)
          {
            cairo_text_extents_t text_extents;

            cairo_save (cr);
            path_slice (cr, x, y, inner, outher, adjustment, segment, segments);

            cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.7);
            if (segment == tools.menu_segment_active)
              ACTIVE_COLOR;
            cairo_fill_preserve (cr);

            cairo_set_line_width (cr, 4.0);
            cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.2);
            cairo_clip_preserve (cr);
            cairo_stroke (cr);
            cairo_restore (cr);

            cairo_new_path (cr);
            cairo_arc (cr, x, y, middle, (segment+0.5)*(3.1415*2)/segments,
                                         (segment+0.5)*(3.1415*2)/segments);
            cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 1.0);

            cairo_save (cr);

            cairo_select_font_face (cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL,
                                                CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size (cr, 40);
            cairo_text_extents (cr, tools.menu_segment_label[segment], &text_extents);
            cairo_rel_move_to (cr, -text_extents.width/2, +text_extents.height/2);
            cairo_show_text (cr, tools.menu_segment_label[segment]);

            cairo_restore (cr);

            if (segment == tools.menu_segment_active)
              {
                cairo_select_font_face (cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL,
                                                    CAIRO_FONT_WEIGHT_NORMAL);
                cairo_set_font_size (cr, 15);
                cairo_text_extents (cr, tools.menu_segment_userdata[segment], &text_extents);
                cairo_rel_move_to (cr, -text_extents.width/2, +text_extents.height/2 + 20);
                cairo_show_text (cr, tools.menu_segment_userdata[segment]);
              }
          }
      }


      cairo_destroy (cr);
    }

  return FALSE;
}

static gboolean
stroke_press_event (GtkWidget      *widget,
                    GdkEventButton *event,
                    gpointer        data)
{
  gint   x, y;
  gdouble scale;
  gdouble tx, ty;
  gdouble ex, ey;

  g_object_get (G_OBJECT (widget),
                "x", &x,
                "y", &y,
                "scale", &scale,
                NULL);

    {
  GeglNode *stroke;
  GeglColor *color = gegl_color_new ("black");

  /* if our parent is an over op, insert our own over op before
   * that over op
   */
  GeglNode *self = tools.node;
  GeglNode *parent = gegl_parent (self);

  if (parent && g_str_equal (gegl_node_get_operation (parent), "gegl:over"))
    {
      select_node (parent);
    }

  gegl_add_sibling ("gegl:over");
  stroke = gegl_add_child ("gegl:path");

    {
      GeglColor *color2;
      gdouble    linewidth;
      gfloat r,g,b,a;
  
      if (g_str_equal (gegl_node_get_operation (self), "gegl:path"))
        {
          gegl_node_get (self, "stroke", &color2, "stroke-width", &linewidth, NULL);
          gegl_color_get_rgba (color2, &r, &g, &b, &a);
          gegl_color_set_rgba (color, r,g,b,a);
        }
      else
        {
          linewidth = 20;
        }

      gegl_node_set (stroke, "d", tools.path=gegl_path_new (), "stroke", color, "stroke-width", linewidth, NULL);
      tools.node = stroke;
    }
    }


  gegl_node_get_translation (GEGL_NODE (tools.node), &tx, &ty);

  ex = (event->x + x) / scale - tx;
  ey = (event->y + y) / scale - ty;

  gegl_path_clear (tools.path);
  gegl_path_append (tools.path, 'M', ex, ey);
  tools.in_drag = TRUE;
#if USE_DYNAMICS
  tools.width_path = gegl_path_add_parameter_path (tools.path, "linewidth");
#endif

  property_editor_rebuild (editor.property_editor, tools.node);

  return TRUE;
}

static gboolean
stroke_release_event (GtkWidget      *widget,
                      GdkEventButton *event,
                      gpointer        dataa)
{
  tools.in_drag = FALSE;
  return FALSE;
}

static gboolean
stroke_motion_notify_event (GtkWidget      *widget,
                            GdkEventMotion *event,
                            gpointer        data)
{
  static gint foo = 0;
  if (tools.in_drag)
    {
      gint   x, y;
      gdouble scale;
      gdouble tx, ty;
      gdouble ex, ey;
      gdouble rx, ry;
      foo ++;

      g_object_get (G_OBJECT (widget),
                    "x", &x,
                      "y", &y,
                      "scale", &scale,
                      NULL);
      gegl_node_get_translation (tools.node, &tx, &ty);

      ex = (event->x + x) / scale - tx;
      ey = (event->y + y) / scale - ty;

      rx = tools.prevx - ex;
      ry = tools.prevy - ey;

      gegl_path_append (tools.path, 'L', ex, ey);

#if USE_DYNAMICS
      if(foo%3==0){
        gdouble magnitude = 1.0;

        gdouble rt = (event->time - tools.prevtime)/1000.0;
        gdouble speed = sqrt (rx*rx + ry*ry)/rt;
#define MAXS 400

        if (speed > MAXS )
          speed = MAXS ;
        magnitude = 1.0-(speed / MAXS);
        if (magnitude > 1.0)
          magnitude = 1.0;

        magnitude = pow (magnitude, 0.2);
        if (magnitude < 0.05)
          magnitude = 0.05;

        gegl_path_append (tools.width_path, '_', -1.0, magnitude);
      }
#endif

      tools.prevx = ex;
      tools.prevy = ey;
      tools.prevtime = event->time;
      return TRUE;

    }
  return FALSE;
}


static gboolean
gui_press_event (GtkWidget      *widget,
                    GdkEventButton *event,
                    gpointer        data)
{
  if (tools.menu_active)
    {
        {
          if (event->button==1)
            {
              gint active = tools.menu_segment_active;
              if (active >= 0)
                {
                  void (*cb) (gpointer) = (void*)tools.menu_segment_callback[active];

                  if (cb)
                   cb (tools.menu_segment_userdata[active]);
                }
            }
          gtk_widget_queue_draw (widget);
          tools.menu_active = FALSE;
          return TRUE;
        }
    }
  if (event->button == 3)
    {
      if (tools.menu_active)
        {
          gint active = tools.menu_segment_active;

          tools.menu_active = FALSE;
          if (active >= 0)
            {
              void (*cb) (gpointer) = (void*)tools.menu_segment_callback[active];

              if (cb)
               cb (tools.menu_segment_userdata[active]);
            }
          gtk_widget_queue_draw (widget);
          tools.menu_active = FALSE;
        }
      else
        {
          gint   x, y;
          gdouble scale;

          tools.menu_active = TRUE;
          g_object_get (G_OBJECT (widget),
                        "x", &x,
                        "y", &y,
                        "scale", &scale,
                      NULL);
  
            tools.menux = (event->x + x);
            tools.menuy = (event->y + y);
            menu_clear ();

            
            switch (tools.state)
              {
                case STATE_MOVE: /* � ￼ */
                  menu_add ("✐", G_CALLBACK (do_command), "set-state strokes");
                  menu_add ("~",  G_CALLBACK (do_command), "set-state edit-nodes");
                  menu_add ("↓",  G_CALLBACK (do_command), "lower");
                  menu_add ("↑",  G_CALLBACK (do_command), "raise-item");
                  menu_add ("☠",  G_CALLBACK (do_command), "remove-item");
                  /* check the current curve type,. */
                  break;
                case STATE_EDIT_NODES:
                  menu_add ("+", G_CALLBACK (do_command), "insert-node");
                  menu_add ("☠", G_CALLBACK (do_command), "remove-node");
                  menu_add ("⚡",  G_CALLBACK (do_command), "set-state edit-width");
                  menu_add ("S",  G_CALLBACK (do_command), "path-smoothen");
                  menu_add ("✜",  G_CALLBACK (do_command), "set-state move");
                  break;
                case STATE_STROKES:
                  menu_add ("~",  G_CALLBACK (do_command), "set-state edit-nodes");
                  menu_add ("✜",  G_CALLBACK (do_command), "set-state move");
                  break;
                case STATE_EDIT_WIDTH:
                  menu_add ("✜",  G_CALLBACK (do_command), "set-state move");
                  menu_add ("✍", G_CALLBACK (do_command), "set-state strokes");
                  break;
                case STATE_EDIT_OPACITY:
                  menu_add ("✜",  G_CALLBACK (do_command), "set-state move");
                  menu_add ("✍", G_CALLBACK (do_command), "set-state strokes");
                  break;
                case STATE_FREE_REPLACE:
                default:
                  menu_add ("✜",  G_CALLBACK (do_command), "set-state move");
                  menu_add ("✍", G_CALLBACK (do_command), "set-state strokes");
                  break;
              }

          }
      gtk_widget_queue_draw (widget);
      return TRUE;
    }

  switch (tools.state)
    {
      case STATE_PICK:
      case STATE_MOVE:
        {
          GeglNode *node;
          GeglNode *detected;
          gint   x, y;
          gdouble scale;
          gdouble tx, ty, ex, ey;

          g_object_get (G_OBJECT (widget),
                        "x", &x,
                        "y", &y,
                        "scale", &scale,
                        "node", &node,
                      NULL);
          detected = gegl_node_detect (node, (x + event->x) / scale,
                                             (y + event->y) / scale);



          gegl_node_get_translation (GEGL_NODE (tools.node), &tx, &ty);

          ex = (event->x + x) / scale;
          ey = (event->y + y) / scale;

          tools.prevx = ex;
          tools.prevy = ey;
          tools.prevtime = event->time;

          g_object_unref (node);
          if (detected)
            {
              tree_editor_set_active (editor.tree_editor, detected);
            }
          tools.in_drag = TRUE;
        }
        break;
      case STATE_EDIT_NODES:
        return nodes_press_event (widget, event, data);
      case STATE_EDIT_WIDTH:
        return width_press_event (widget, event, data);
      case STATE_STROKES:
        if (!tools.node)
          return FALSE;
        return stroke_press_event (widget, event, data);
      case STATE_EDIT_OPACITY:
      case STATE_FREE_REPLACE:
      default:
        g_warning ("not handling top release of state %i\n", tools.state);
        break;
    }

  return FALSE;
}


static gboolean
gui_motion_event (GtkWidget      *widget,
                  GdkEventMotion *event,
                  gpointer        data)
{
  if (tools.menu_active)
    {
      gint segment;
      gint segments = tools.menu_segments;

      gint inner = 35;
      gint outher = 100;
      gdouble adjustment = 3.0; /* adjustments to make the margin produce
                                   a straight line from the centre
                                   (set by trial and error) */

      gint x, y;
      gdouble scale;
      cairo_t *cr = gdk_cairo_create (widget->window);
      g_object_get (G_OBJECT (widget),
                    "x", &x,
                    "y", &y,
                    "scale", &scale,
                    NULL);

      {
        gboolean found_it = FALSE;
        for (segment=0;segment<segments;segment++)
          {
            path_slice (cr, tools.menux, tools.menuy, inner/3, outher, adjustment, segment, segments);

            if (cairo_in_fill (cr, event->x + x, event->y + y))
              {
                if (tools.menu_segment_active != segment)
                  {
                    tools.menu_segment_active = segment;
                    gtk_widget_queue_draw (widget);
                  }
               found_it = TRUE;
               break;
             }
          }
        if (!found_it)
          {
            if (tools.menu_segment_active != -1)
              gtk_widget_queue_draw (widget);
            tools.menu_segment_active = -1;
          }
      }
      cairo_destroy (cr);
    }

  switch (tools.state)
    {
      case STATE_PICK:
        break;
      case STATE_MOVE:
        if (tools.in_drag)
          {
            gint   x, y;
            gdouble scale;
            gdouble tx, ty, ex, ey;

            g_object_get (G_OBJECT (widget),
                          "x", &x,
                          "y", &y,
                          "scale", &scale,
                          NULL);

            gegl_node_get_translation (GEGL_NODE (tools.node), &tx, &ty);

            ex = (event->x + x) / scale;
            ey = (event->y + y) / scale;

            move_rel (tools.node, ex-tools.prevx, ey-tools.prevy);


            tools.prevx = ex;
            tools.prevy = ey;
          }
        break;
      case STATE_STROKES:
        return stroke_motion_notify_event (widget, event, data);
      case STATE_EDIT_NODES:
        return nodes_motion_notify_event (widget, event, data);
      case STATE_EDIT_WIDTH:
        return width_motion_notify_event (widget, event, data);
      case STATE_EDIT_OPACITY:
      case STATE_FREE_REPLACE:
      default:
        g_warning ("not handling top motion of state %i\n", tools.state);
        break;
    }
  return FALSE;
}


static gboolean
gui_release_event (GtkWidget      *widget,
                   GdkEventButton *event,
                   gpointer        data)
{
  if (tools.menu_active)
      {
#if 0
        gint active = tools.menu_segment_active;

        tools.menu_active = FALSE;
        if (active >= 0)
          {
            void (*cb) (gpointer) = (void*)tools.menu_segment_callback[active];

            if (cb)
             cb (tools.menu_segment_userdata[active]);
          }
        gtk_widget_queue_draw (widget);
#endif
      }

  switch (tools.state)
    {
      case STATE_MOVE:
        tools.in_drag=FALSE;
        break;
      case STATE_PICK:
        break;
      case STATE_EDIT_NODES:
        return nodes_release_event (widget, event, data);
      case STATE_STROKES:
        return stroke_release_event (widget, event, data);
      case STATE_EDIT_WIDTH:
        return width_release_event (widget, event, data);
      case STATE_EDIT_OPACITY:
      case STATE_FREE_REPLACE:
      default:
        g_warning ("not handling top release of state %i\n", tools.state);
        break;
    }
  return FALSE;
}

void editor_set_active (gpointer view, gpointer node);
void editor_set_active (gpointer view, gpointer node)
{
  const gchar *opname;

  if (!view || ! node)
    return;

  opname = gegl_node_get_operation (node);
  tools.node = node;

  if (g_str_equal (opname, "gegl:path"))
    {
      GeglPath *vector;
      gegl_node_get (node, "d", &vector, NULL);

      tools.path = vector;

      if (vector)
        g_object_unref (vector);
    }
  else
    {
      tools.path = NULL;
    }
  gtk_widget_queue_draw (GTK_WIDGET (view));
}

static void editor_set_gegl (GeglNode    *gegl);
static GtkWidget *create_menubar (Editor *editor);
static GtkWidget *
create_window (Editor *editor)
{
  GtkWidget *self;
  GtkWidget *vbox;
  GtkWidget *hbox;
  GtkWidget *vbox2;
  GtkWidget *menubar;
  GtkWidget *hpaned_top_level;
  GtkWidget *hpaned_top;
  GtkWidget *vpaned;
  GtkWidget *view;
  GtkWidget *property_scroll;
  GtkWidget *add_box;
  GtkWidget *add_entry;

  /* creation of ui components */
  editor->window = self = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  vbox = gtk_vbox_new (FALSE, 1);
  hbox = gtk_hbox_new (FALSE, 1);
  vbox2 = gtk_vbox_new (FALSE, 1);
  hpaned_top = gtk_vpaned_new ();
  hpaned_top_level = gtk_hpaned_new ();
  view = g_object_new (GEGL_TYPE_VIEW, NULL, "block", TRUE, NULL);
  property_scroll = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_add_with_viewport (GTK_SCROLLED_WINDOW (property_scroll), editor->property_editor);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (property_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  vpaned = gtk_vpaned_new ();

  menubar = create_menubar (editor); /*< depends on other widgets existing */

  add_box = gtk_hbox_new (FALSE, 1);
  add_entry = gtk_entry_new ();

  /* packing */

  gtk_container_add (GTK_CONTAINER (self), vbox);
  gtk_box_pack_start (GTK_BOX (hbox), menubar, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox), hpaned_top_level, TRUE, TRUE, 0);
  gtk_paned_pack2 (GTK_PANED (hpaned_top_level), hpaned_top, FALSE, TRUE);
  gtk_box_pack_start (GTK_BOX (hbox), gtk_label_new ("     "), FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (hbox), add_box, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (vbox2), view, TRUE, TRUE, 0);
  gtk_paned_pack1 (GTK_PANED (hpaned_top_level), vbox2, TRUE, TRUE);

  gtk_paned_pack1 (GTK_PANED (hpaned_top), property_scroll, FALSE,
                   TRUE);
  gtk_paned_pack2 (GTK_PANED (hpaned_top), editor->tree_editor, FALSE, TRUE);

    {
      GtkWidget *foo = gegl_typeeditor_optype (NULL, NULL, NULL);
      gtk_box_pack_start (GTK_BOX (add_box), foo, TRUE, TRUE, 0);
    }

  /* setting properties for ui components */
  gtk_window_set_gravity (GTK_WINDOW (self), GDK_GRAVITY_STATIC);
  gtk_window_set_title (GTK_WINDOW (self), "GEGL");
  gtk_widget_set_size_request (editor->tree_editor, -1, 100);
  gtk_widget_set_size_request (property_scroll, -1, 100);
  gtk_widget_set_size_request (view, 89, 55);

  g_signal_connect (self, "delete-event",
                    G_CALLBACK (cb_window_delete_event), NULL);

  g_signal_connect (self, "key-press-event",
                    G_CALLBACK (cb_window_keybinding), NULL);

  gtk_widget_show_all (vbox);

  g_signal_connect_swapped (view, "notify::scale",
                            G_CALLBACK (gegl_editor_update_title),
                            editor);

  editor->view = view;
  editor->structure = hpaned_top;
  editor->property_pane = property_scroll;
  editor->tree_pane = editor->tree_editor;
  gtk_widget_hide (editor->structure);
  return self;
}

GeglNode *editor_output = NULL;

static void cb_shrinkwrap (GtkAction *action);
static void cb_fit (GtkAction *action);
static void cb_fit_on_screen (GtkAction *action);
#if 0
static void cb_recompute (GtkAction *action);
#endif
static void cb_redraw (GtkAction *action);
static void cb_next_file (GtkAction *action);
static void cb_previous_file (GtkAction *action);


static GeglNode *input_stream(GeglNode *root)
{
  GeglNode *iter = gegl_node_get_output_proxy (root, "output");
  gegl_node_get_bounding_box (editor.gegl);  /* to trigger defined setting for all */
  while (iter &&
         gegl_node_get_producer (iter, "input", NULL)){
    iter = gegl_node_get_producer (iter, "input", NULL);
  }
  return iter;
}

/* avoided to make this a really public function, since the need
 * is a bit of a hack
 */
GeglProcessor *gegl_view_get_processor (GeglView *view);

static gboolean play(gpointer data)
{
  Editor *editor=data;
  GeglView *view=GEGL_VIEW (editor->view);
  gdouble   progress;
  g_object_get (gegl_view_get_processor (view), "progress", &progress, NULL);
  if (progress >= 1.0)
    {
      /* modify source to trigger consumption of a new element */
      GeglNode *source = input_stream (editor->gegl);
      gint frame;
      gegl_node_get (source, "frame", &frame, NULL);
      /*g_warning ("(%f) frame: %i->%i", progress, frame, frame +1);*/
      frame++;
      gegl_node_set (source, "frame", frame, NULL);
      gegl_gui_flush ();
    }
  else
    {
      /*g_warning ("(%f)", progress);*/
    }

  return TRUE;
}

static gboolean advance_slide (gpointer data)
{
  cb_next_file (NULL);
  return TRUE;
}

gint
editor_main (GeglNode    *gegl,
             GeglOptions *options)
{
  GtkWidget *treeview;

  g_object_set (gegl_config (), "babl-tolerance", 0.02, NULL);

  editor.options = options;
  editor.property_editor = gtk_vbox_new (FALSE, 0);
  editor.tree_editor = tree_editor_new (editor.property_editor);
  editor.graph_editor = NULL;/*gtk_label_new ("graph");*/
  editor.window = create_window (&editor);
  treeview = tree_editor_get_treeview (editor.tree_editor);
  gtk_container_add (GTK_CONTAINER (editor.property_editor),
                     gtk_label_new (editor.options->file));
  gtk_widget_show (editor.window);
  gtk_container_set_border_width (GTK_CONTAINER (editor.property_editor), 6);

  g_signal_connect_after (editor.view, "expose-event",
                          G_CALLBACK (cairo_gui_expose), NULL);

  g_signal_connect (editor.view, "button-press-event",
                          G_CALLBACK (gui_press_event), NULL);
  g_signal_connect (editor.view, "button-release-event",
                          G_CALLBACK (gui_release_event), NULL);
  g_signal_connect (editor.view, "motion-notify-event",
                          G_CALLBACK (gui_motion_event), NULL);

  tools.state = STATE_EDIT_NODES;
  tools.node = NULL;
  tools.path = NULL;

  editor_set_gegl (gegl);

  /*cb_shrinkwrap (NULL);*/
  cb_fit_on_screen (NULL);
  gegl_editor_update_title ();

  if (options->delay != 0.0)
    {
      g_timeout_add (1000 * options->delay, advance_slide, NULL);
    }
  if (options->play)
    {
      g_timeout_add (100, play, &editor);
    }

  /*gtk_window_fullscreen (GTK_WINDOW (editor.window));*/
  gtk_main ();
  return 0;
}

static void cb_about (GtkAction *action);
/*static void cb_introspect (GtkAction *action);*/
static void cb_export (GtkAction *action);
/*static void cb_flush   (GtkAction *action);*/
static void cb_quit_dialog (GtkAction *action);
static void cb_composition_new (GtkAction *action);
static void cb_composition_load (GtkAction *action);
static void cb_composition_save (GtkAction *action);

static void cb_zoom_in (GtkAction *action);
static void cb_zoom_out (GtkAction *action);
static void cb_zoom_50 (GtkAction *action);
static void cb_zoom_100 (GtkAction *action);
static void cb_zoom_200 (GtkAction *action);


static GtkActionEntry action_entries[] = {
  {"CompositionMenu", NULL, "_Composition", NULL, NULL, NULL},
  {"ViewMenu", NULL, "_View", NULL, NULL, NULL},
  {"HelpMenu", NULL, "_Help", NULL, NULL, NULL},

  {"New", GTK_STOCK_NEW,
   "_New", "<control>N",
   "Create a new composition",
   G_CALLBACK (cb_composition_new)},

  {"Next", GTK_STOCK_GO_FORWARD,
   "_Next", "<control>a",
   "Go to next file in list",
   G_CALLBACK (cb_next_file)},

  {"Previous", GTK_STOCK_GO_BACK,
   "_Previous", "<control>z",
   "Go to previous file in list",
   G_CALLBACK (cb_previous_file)},

  {"Open", GTK_STOCK_OPEN,
   "_Open", "<control>O",
   "Open a composition",
   G_CALLBACK (cb_composition_load)},

  {"Save", GTK_STOCK_SAVE,
   "_Save", "<control>S",
   "Save current composition",
   G_CALLBACK (cb_composition_save)},

  {"Quit", GTK_STOCK_QUIT,
   "_Quit", "<control>Q",
   "Quit",
   G_CALLBACK (cb_quit_dialog)},

  {"About", GTK_STOCK_ABOUT,
   "_About", "",
   "About",
   G_CALLBACK (cb_about)},
/*
  {"Introspect", NULL,
   "_Introspect", "<control>I",
   "Introspect",
   G_CALLBACK (cb_introspect)},*/


  {"Export", GTK_STOCK_SAVE,
   "_Export", "<control><shift>E",
   "Export to PNG",
   G_CALLBACK (cb_export)},
  /*
  {"Flush", GTK_STOCK_SAVE,
   "_Flush", "<control><shift>E",
   "Flush swap buffer",
   G_CALLBACK (cb_flush)},*/

  {"ShrinkWrap", NULL,
   "_Shrink Wrap", "<control>E",
   "Size the window to the image, if feasible",
   G_CALLBACK (cb_shrinkwrap)},

  {"Fit", GTK_STOCK_ZOOM_FIT,
   "_Fit", "<control>F",
   "Fit the image in window",
   G_CALLBACK (cb_fit)},

  {"FitOnScreen", NULL,
   "_Fit On Screen", "",
   "Fit the image on screen",
   G_CALLBACK (cb_fit_on_screen)},

  {"ZoomIn", GTK_STOCK_ZOOM_IN,
   "Zoom In", "<control>plus",
   "",
   G_CALLBACK (cb_zoom_in)},

  {"ZoomOut", GTK_STOCK_ZOOM_OUT,
   "Zoom Out", "<control>minus",
   "",
   G_CALLBACK (cb_zoom_out)},

  {"Zoom50", NULL,
   "50%", "<control>2",
   "",
   G_CALLBACK (cb_zoom_50)},

  {"Zoom100", GTK_STOCK_ZOOM_100,
   "100%", "<control>1",
   "",
   G_CALLBACK (cb_zoom_100)},

  {"Zoom200", NULL,
   "200%", "",
   "",
   G_CALLBACK (cb_zoom_200)},

#if 0
  {"Recompute", NULL,
   "_Recompute View", "<shift><control>R",
   "Recalculate all image data (for working around dirt bugs)",
   G_CALLBACK (cb_recompute)},
#endif

  {"Redraw", NULL,
   "_Redraw View", "<control>R",
   "Repaints all image data (works around display glitches)",
   G_CALLBACK (cb_redraw)},
};
static guint n_action_entries = G_N_ELEMENTS (action_entries);

static const gchar *ui_info =
  "<ui>"
  "  <menubar name='MenuBar'>"
  "    <menu action='CompositionMenu'>"
  "      <separator/>"
  "      <menuitem action='New'/>"
  "      <menuitem action='Open'/>"
  "      <menuitem action='Save'/>"
  "      <separator/>"
  "      <menuitem action='Next'/>"
  "      <menuitem action='Previous'/>"
  "      <separator/>"
  "      <menuitem action='Export'/>"
  /*"      <menuitem action='Flush'/>"*/
  "      <separator/>"
  "      <menuitem action='Quit'/>"
  "      <separator/>"
  "    </menu>"
  "    <menu action='ViewMenu'>"
  "      <menuitem action='FitOnScreen'/>"
  "      <menuitem action='Fit'/>"
  "      <menuitem action='ShrinkWrap'/>"
  "      <separator/>"
  "      <menuitem action='ZoomIn'/>"
  "      <menuitem action='ZoomOut'/>"
  "      <menuitem action='Zoom50'/>"
  "      <menuitem action='Zoom100'/>"
  "      <menuitem action='Zoom200'/>"
  "      <separator/>"
  "      <menuitem action='Redraw'/>"
  /*"      <menuitem action='Recompute'/>"*/
  "      <separator/>"
  "      <menuitem action='Structure'/>"
  "      <menuitem action='Tree'/>"
  "      <menuitem action='Properties'/>"
  /*"      <menuitem action='Introspect'/>"*/
  "    </menu>"
  "    <menu action='HelpMenu'>"
  "      <menuitem action='About'/>"
  "    </menu>"
  "  </menubar>"
  "</ui>";

static void cb_tree_visible (GtkAction *action, gpointer userdata);
static void cb_properties_visible (GtkAction *action, gpointer userdata);
static void cb_structure_visible (GtkAction *action, gpointer userdata);

static GtkToggleActionEntry toggle_entries[]={
    {"Tree", NULL,
     "TreeView", NULL,
     "Toggle visibility of tree structure of composition",
     G_CALLBACK (cb_tree_visible),
     TRUE},
    {"Properties", NULL,
     "PropertiesView", NULL,
     "Toggle visibility of property editor",
     G_CALLBACK (cb_properties_visible),
     TRUE},
    {"Structure", NULL,
     "StructureView", "F5",
     "Toggle visibility of sidebar",
     G_CALLBACK (cb_structure_visible),
     FALSE},
};

static guint n_toggle_entries = G_N_ELEMENTS (toggle_entries);

static GtkActionGroup *get_actions(void)
{
  static GtkActionGroup *actions = NULL;
  if (!actions)
    {
      actions = gtk_action_group_new ("Actions");
      gtk_action_group_add_actions (actions, action_entries, n_action_entries,
                                    NULL);
      gtk_action_group_add_toggle_actions (actions, toggle_entries, n_toggle_entries,
                                           (void*)0x6367); /* <- probably userdata */
    }
  return actions;
}

static GtkWidget *
create_menubar (Editor *editor)
{
  GtkUIManager *ui;
  GError   *error = NULL;

  ui = gtk_ui_manager_new ();
  gtk_ui_manager_set_add_tearoffs (ui, TRUE);
  gtk_ui_manager_insert_action_group (ui, get_actions (), 0);

  gtk_window_add_accel_group (GTK_WINDOW (editor->window),
                              gtk_ui_manager_get_accel_group (ui));

  if (!gtk_ui_manager_add_ui_from_string (ui, ui_info, -1, &error))
    {
      g_message ("building menus failed: %s", error->message);
      g_error_free (error);
    }

  return gtk_ui_manager_get_widget (ui, "/MenuBar");
}

static void
cb_composition_new (GtkAction *action)
{
  GtkWidget *dialog;
  GtkWidget *hbox;
  GtkWidget *alert;
  GtkWidget *label;
  gint      result;

  dialog = gtk_dialog_new_with_buttons ("GEGL - New Composition",
                                        GTK_WINDOW (editor.window),
                                        GTK_DIALOG_MODAL,
                                        GTK_STOCK_CANCEL, GTK_RESPONSE_REJECT,
                                        GTK_STOCK_OK, GTK_RESPONSE_ACCEPT,
                                        NULL);
  gtk_dialog_set_has_separator (GTK_DIALOG (dialog), FALSE);

  hbox = gtk_hbox_new (FALSE, 12);
  gtk_container_set_border_width (GTK_CONTAINER (hbox), 12);
  gtk_container_add (GTK_CONTAINER (GTK_DIALOG (dialog)->vbox), hbox);

  alert = gtk_image_new_from_stock (GTK_STOCK_DIALOG_WARNING,
                                    GTK_ICON_SIZE_DIALOG);
  gtk_container_add (GTK_CONTAINER (hbox), alert);

  label = gtk_label_new ("Discard current composition?\n"
                         "All unsaved data will be lost.");
  gtk_container_add (GTK_CONTAINER (hbox), label);

  gtk_widget_show_all (dialog);
  result = gtk_dialog_run (GTK_DIALOG (dialog));

  switch (result)
    {
    case GTK_RESPONSE_ACCEPT:
      {
      /* FIXME: should append to list of files, and set as current
        editor_set_gegl (gegl_parse_xml (blank_composition), "untitled.xml");
        */
        editor_set_gegl (gegl_node_new_from_xml (blank_composition, "/"));
      }
      break;
    default:
      break;
    }
  gtk_widget_destroy (dialog);
}

static gboolean
cb_window_delete_event (GtkWidget *widget, GdkEvent *event, gpointer data)
{
  cb_quit_dialog (NULL);
  return TRUE;
}

static void
cb_composition_load (GtkAction *action)
{
  GtkWidget *dialog;
  GtkFileFilter *filter;

  dialog = gtk_file_chooser_dialog_new ("Load GEGL Composition",
                                        GTK_WINDOW (editor.window),
                                        GTK_FILE_CHOOSER_ACTION_OPEN,
                                        GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                        GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
                                        NULL);

  filter = gtk_file_filter_new ();
  gtk_file_filter_add_mime_type (filter, "text/xml");
  gtk_file_filter_set_name (filter, "GEGL composition");
  gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (dialog), filter);

  gtk_file_chooser_add_shortcut_folder (GTK_FILE_CHOOSER (dialog),
                                        "/home/pippin/src/editor/", NULL);

  if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT)
    {
      const gchar *filename;
      gchar *xml;

      filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (dialog));

      g_file_get_contents (filename, &xml, NULL, NULL);
      /* FIXME: append name to list of files in options */
      /* FIXME: handle non XML files */
        {
          gchar *temp = g_strdup (filename);
          gchar *path = g_strdup (g_path_get_dirname (temp));
          editor_set_gegl (gegl_node_new_from_xml (xml, path));
          g_free (temp);
          g_free (path);
        }
      g_free (xml);
    }
  gtk_widget_destroy (dialog);
}

static void
cb_composition_save (GtkAction *action)
{

  GtkWidget *dialog;
  GtkFileFilter *filter;

  dialog = gtk_file_chooser_dialog_new ("Save GEGL Composition",
                                        GTK_WINDOW (editor.window),
                                        GTK_FILE_CHOOSER_ACTION_SAVE,
                                        GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                        GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT,
                                        NULL);

  filter = gtk_file_filter_new ();
  gtk_file_filter_add_mime_type (filter, "text/xml");
  gtk_file_filter_set_name (filter, "GEGL composition");
  gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (dialog), filter);

  gtk_file_chooser_add_shortcut_folder (GTK_FILE_CHOOSER (dialog),
                                        "/home/pippin/media/video/", NULL);

  {
    gchar absolute_path[PATH_MAX];
    realpath (editor.options->file, absolute_path);
    gtk_file_chooser_set_filename (GTK_FILE_CHOOSER (dialog), absolute_path);
  }

  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);

  if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT)
    {
      const gchar *filename =
        gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (dialog));
      if (filename)
        {
          gchar *full_filename;
          gchar  abs_filepath[PATH_MAX];
          gchar *abs_path;
          gchar *xml;

          if (strstr (filename, "xml"))
            {
              full_filename = strdup (filename);
            }
          else
            {
              full_filename = g_malloc (strlen (filename) + 4);
              strcpy (full_filename, filename);
              full_filename = strcat (full_filename, ".xml");
            }
          /*
           * FIXME: append new path to end of list or something and set as current,..
          if (editor.options->file)
            g_free (editor.options->file);
          editor.options->file = g_strdup (full_filename);
          */

          realpath (full_filename, abs_filepath);
          abs_path = g_strdup (g_path_get_dirname (abs_filepath));
          xml = gegl_node_to_xml (editor.gegl, abs_path); /*oxide_xml (editor->oxide, abs_path);*/

          g_file_set_contents (full_filename, xml, -1, NULL);

          g_free (abs_path);
          g_free (xml);
          g_free (full_filename);
        }
    }
  gtk_widget_destroy (dialog);
}

static void
cb_quit_dialog (GtkAction *action)
{
  GtkWidget *dialog;
  GtkWidget *label;
  GtkWidget *hbox;
  GtkWidget *alert;

  dialog = gtk_dialog_new_with_buttons ("GEGL - Confirm Quit",
                                        GTK_WINDOW (editor.window),
                                        GTK_DIALOG_MODAL,
                                        GTK_STOCK_CANCEL, GTK_RESPONSE_REJECT,
                                        GTK_STOCK_SAVE,   4,
                                        GTK_STOCK_QUIT,   GTK_RESPONSE_ACCEPT,
                                        NULL);
  gtk_dialog_set_has_separator (GTK_DIALOG (dialog), FALSE);

  hbox = gtk_hbox_new (FALSE, 12);
  gtk_container_set_border_width (GTK_CONTAINER (hbox), 12);
  gtk_container_add (GTK_CONTAINER (GTK_DIALOG (dialog)->vbox), hbox);

  alert = gtk_image_new_from_stock (GTK_STOCK_DIALOG_WARNING,
                                    GTK_ICON_SIZE_DIALOG);
  gtk_container_add (GTK_CONTAINER (hbox), alert);

  label = gtk_label_new ("Really quit?\n"
                         "All unsaved data will be lost.");
  gtk_container_add (GTK_CONTAINER (hbox), label);

  gtk_widget_show_all (hbox);

  switch (gtk_dialog_run (GTK_DIALOG (dialog)))
    {
    case GTK_RESPONSE_ACCEPT:
      gtk_main_quit ();
      break;
    case 4:
      cb_composition_save (action);
    default:
      break;
    }

  gtk_widget_destroy (dialog);
}

static gboolean file_is_gegl_xml (const gchar *path)
{
  gchar *extension;

  extension = strrchr (path, '.');
  if (!extension)
    return FALSE;
  extension++;
  if (extension[0]=='\0')
    return FALSE;
  if (!strcmp (extension, "xml")||
      !strcmp (extension, "XML"))
    return TRUE;
  return FALSE;
}

static void do_load (void)
{
  GeglOptions *o = editor.options;
  gchar *xml = NULL;

  gchar *temp1 = g_strdup (o->file);
  gchar *temp2;
  gchar *path_root;
  temp2 = g_strdup (g_path_get_dirname (temp1));
  path_root = g_strdup (realpath (temp2, NULL));
  g_free (temp1);
  g_free (temp2);

  if (file_is_gegl_xml (o->file))
    {
      GError *err = NULL;

      g_file_get_contents (o->file, &xml, NULL, &err);

      if (err != NULL)
        {
          g_printerr ("Unable to read file: %s", err->message);
          g_error_free (err);
        }
    }
  else
    {
      GString *acc = g_string_new ("");

      {
        gchar *basename = g_path_get_basename (o->file);

        g_string_append (acc, "<gegl><load path='");
        g_string_append (acc, basename);
        g_string_append (acc, "'/></gegl>");

        g_free (basename);
      }

      xml = g_string_free (acc, FALSE);
    }

  editor_set_gegl (gegl_node_new_from_xml (xml, path_root));
  g_free (path_root);
  g_free (xml);
}

static void cb_next_file (GtkAction *action)
{
  gegl_options_next_file (editor.options);
  do_load ();
  cb_fit (NULL);
}


static void cb_previous_file (GtkAction *action)
{
  gegl_options_previous_file (editor.options);
  do_load ();
  cb_fit (NULL);
}


static void
cb_about (GtkAction *action)
{
  GtkWidget *window;
  GtkWidget *about;
  GeglNode  *gegl;

  gegl = gegl_node_new_from_xml (
   "<gegl> <over> <invert/> <shift x='20.0' y='140.0'/> <text string=\"GEGL is a image processing and compositing framework.\n\nGUI editor Copyright © 2006, 2007 Øyvind Kolås, Kevin Cozens, Sven Neumann and Michael Schumacher\nGEGL and its editor come with ABSOLUTELY NO WARRANTY. This is free software, and you are welcome to redistribute it under certain conditions. The processing and compositing library GEGL is licensed under LGPLv3+ and the editor itself is licensed as GPLv3+.\" font='Sans' size='10.0' wrap='300' alignment='0' width='224' height='52'/> </over> <over> <shift x='20.0' y='10.0'/> <dropshadow opacity='1.0' x='10.0' y='10.0' radius='5.0'/> <text string='GEGL' font='Sans' size='100.0' wrap='-1' alignment='0'/> </over> <perlin-noise alpha='12.30' scale='0.10' zoff='-1.0' seed='20.0' n='6.0'/> </gegl>"
  ,NULL);

  window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title (GTK_WINDOW (window), "About GEGL");
  about = g_object_new (GEGL_TYPE_VIEW,
                        "node", gegl,
                        NULL);
  g_object_unref (gegl);
  gtk_container_add (GTK_CONTAINER (window), about);
  gtk_widget_set_size_request (about, 320, 260);

  g_signal_connect (G_OBJECT (window), "delete-event",
                    G_CALLBACK (gtk_widget_destroy), window);
  gtk_widget_show_all (window);
}

/*
static void
cb_introspect (GtkAction *action)
{
  GtkWidget *window;
  GtkWidget *introspect;
  GeglNode  *gegl;
  GeglNode  *dot;
  GeglRectangle bounding_box;

  gegl = gegl_node_new ();
  dot = gegl_node_new_child (gegl,
                             "operation", "gegl:introspect",
                             "node", editor.gegl,
                             NULL);
  window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title (GTK_WINDOW (window), "GEGL Introspection");
  introspect = g_object_new (GEGL_TYPE_VIEW,
                             "node", dot,
                             NULL);
  bounding_box = gegl_node_get_bounding_box (dot);
  bounding_box = gegl_node_get_bounding_box (dot);
  g_object_unref (gegl);

  gtk_container_add (GTK_CONTAINER (window), introspect);

  gtk_widget_set_size_request (introspect, bounding_box.width  * 0.70, bounding_box.height * 0.70);
  g_object_set (introspect, "scale", 0.70, NULL);

  g_signal_connect (G_OBJECT (window), "delete-event",
                    G_CALLBACK (gtk_widget_destroy), window);
  gtk_widget_show_all (window);

}*/

static void cb_structure_visible (GtkAction *action, gpointer userdata)
{
  GtkWidget *widget = editor.structure;

  if (GTK_WIDGET_VISIBLE (widget))
    {
      gtk_widget_hide (widget);
    }
  else
    {
      gtk_widget_show (widget);
    }
}

static void cb_properties_visible (GtkAction *action, gpointer userdata)
{
  GtkWidget *widget = editor.property_pane;
  if (GTK_WIDGET_VISIBLE (widget))
    {
      gtk_widget_hide (widget);
    }
  else
    {
      gtk_widget_show (widget);
    }
}

static void cb_tree_visible (GtkAction *action, gpointer userdata)
{
  GtkWidget *widget = editor.tree_pane;
  if (GTK_WIDGET_VISIBLE (widget))
    {
      gtk_widget_hide (widget);
    }
  else
    {
      gtk_widget_show (widget);
    }
}

static void cb_fit (GtkAction *action)
{
  GeglRectangle defined = gegl_node_get_bounding_box (editor.gegl);
  gint          x, y;
  gdouble       hscale;
  gdouble       vscale;

  hscale = (gdouble) editor.view->allocation.width / defined.width ;
  vscale = (gdouble) editor.view->allocation.height / defined.height ;

  if (hscale > vscale)
    {
      hscale = vscale;
      y=0;
      x= (editor.view->allocation.width - defined.width  * hscale) / 2 / hscale;
    }
  else
    {
      vscale = hscale;
      x=0;
      y= (editor.view->allocation.height - defined.height  * vscale) / 2 / vscale;
    }

  g_object_set (editor.view,
                "x",     defined.x - x,
                "y",     defined.y - y,
                "scale", hscale,
                NULL);
}


static void cb_fit_on_screen (GtkAction *action)
{
  GeglRectangle defined = gegl_node_get_bounding_box (editor.gegl);
  gint ow = editor.view->allocation.width;

  g_object_set (editor.view, "x", defined.x, "y", defined.y, NULL);
  {
    GdkScreen *screen= gtk_window_get_screen (GTK_WINDOW (editor.window));

    gint    screen_width, screen_height;

    screen_width = gdk_screen_get_width (screen);
    screen_height = gdk_screen_get_height (screen);

    gtk_window_resize (GTK_WINDOW (editor.window), screen_width * 0.75,
                                                   screen_height * 0.75);
    while (ow == editor.view->allocation.width)
      gtk_main_iteration();
  }
  cb_fit (action);
  cb_shrinkwrap (action);
}

static void cb_shrinkwrap (GtkAction *action)
{
  GeglRectangle defined = gegl_node_get_bounding_box (editor.gegl);
  /*g_warning ("shrink wrap %i,%i %ix%i", defined.x, defined.y, defined.width , defined.h);*/

  g_object_set (editor.view, "x", defined.x, "y", defined.y, NULL);
  {
    GdkScreen *screen= gtk_window_get_screen (GTK_WINDOW (editor.window));

    gint    screen_width, screen_height;
    gint    width, height;
    gdouble scale;
    g_object_get (editor.view, "scale", &scale, NULL);

    /* compute a base width/height for the other contents of the window */
    screen_width = gdk_screen_get_width (screen);
    screen_height = gdk_screen_get_height (screen);
    gtk_window_get_size (GTK_WINDOW (editor.window), &width, &height);
    width -= editor.view->allocation.width;
    height -= editor.view->allocation.height;

    /* add the area consumed by the canvas content */
    width += defined.width  * scale;
    height += defined.height  * scale;

    if (width > screen_width)
      width = screen_width;
    if (height > screen_height)
      height = screen_height;

    gtk_window_resize (GTK_WINDOW (editor.window), width, height);
  }
/*
  for (i=0;i<40;i++){
    gtk_main_iteration ();
  }*/
}

#if 0
static void cb_recompute (GtkAction *action)
{
  GeglNode *node;

  g_object_get (GEGL_VIEW(editor.view), "node", &node, NULL);
  /*gegl_node_disable_cache (node);*/
  this used to just forcibly remove the existing cache object for the
    toplevel cache.
  */
  gegl_gui_flush ();
}
#endif

static void cb_redraw (GtkAction *action)
{
  gtk_widget_queue_draw (editor.view);
}

static void gegl_editor_update_title (void)
{
  gdouble zoom;
  gchar buf[512];
  g_object_get (editor.view, "scale", &zoom, NULL);
  sprintf (buf, "GEGL %2.0f%%", zoom * 100);

  gtk_window_set_title (GTK_WINDOW (editor.window), buf);
}

static void cb_zoom_100 (GtkAction *action)
{
  gint width, height;
  gint x,y;
  gdouble scale;

  width = editor.view->allocation.width;
  height = editor.view->allocation.height;

  g_object_get (editor.view,
                "x", &x,
                "y", &y,
                "scale", &scale,
                NULL);

  x += (width/2) / scale;
  y += (height/2) / scale;

  scale = 1.0;

  x -= (width/2) / scale;
  y -= (height/2) / scale;

  g_object_set (editor.view,
                "x", x,
                "y", y,
                "scale", scale,
                NULL);
}

static void cb_zoom_200 (GtkAction *action)
{
  gint width, height;
  gint x,y;
  gdouble scale;

  width = editor.view->allocation.width;
  height = editor.view->allocation.height;

  g_object_get (editor.view,
                "x", &x,
                "y", &y,
                "scale", &scale,
                NULL);

  x += (width/2) / scale;
  y += (height/2) / scale;

  scale = 2.0;

  x -= (width/2) / scale;
  y -= (height/2) / scale;

  g_object_set (editor.view,
                "x", x,
                "y", y,
                "scale", scale,
                NULL);
}

static void cb_zoom_50 (GtkAction *action)
{
  gint width, height;
  gint x,y;
  gdouble scale;

  width = editor.view->allocation.width;
  height = editor.view->allocation.height;

  g_object_get (editor.view,
                "x", &x,
                "y", &y,
                "scale", &scale,
                NULL);

  x += (width/2) / scale;
  y += (height/2) / scale;

  scale = 0.5;

  x -= (width/2) / scale;
  y -= (height/2) / scale;

  g_object_set (editor.view,
                "x", x,
                "y", y,
                "scale", scale,
                NULL);
}


static void cb_zoom_in (GtkAction *action)
{
  gint    width, height;
  gint    x, y;
  gdouble scale;
  gdouble focus_x, focus_y;

  width  = editor.view->allocation.width;
  height = editor.view->allocation.height;

  g_object_get (editor.view,
                "x",     &x,
                "y",     &y,
                "scale", &scale,
                NULL);

  focus_x = (x + width  / 2) / scale;
  focus_y = (y + height / 2) / scale;

  scale *= KEY_ZOOM_FACTOR;

  x = focus_x * scale - width  / 2;
  y = focus_y * scale - height / 2;

  g_object_set (editor.view,
                "x",     x,
                "y",     y,
                "scale", scale,
                NULL);
}

static void cb_zoom_out (GtkAction *action)
{
  gint    width, height;
  gint    x, y;
  gdouble scale;
  gdouble focus_x, focus_y;

  width  = editor.view->allocation.width;
  height = editor.view->allocation.height;

  g_object_get (editor.view,
                "x",     &x,
                "y",     &y,
                "scale", &scale,
                NULL);

  focus_x = (x + width  / 2) / scale;
  focus_y = (y + height / 2) / scale;

  scale /= KEY_ZOOM_FACTOR;

  x = focus_x * scale - width  / 2;
  y = focus_y * scale - height / 2;

  g_object_set (editor.view,
                "x",     x,
                "y",     y,
                "scale", scale,
                NULL);

  gegl_gui_flush ();
}

#if 1
#include "export.h"

static void cb_export (GtkAction *action)
{
  export_window ();
}
#endif

#include "gegl-plugin.h"

#if 0
#include "graph/gegl-node.h" /*< FIXME: including internal header */
#endif

#if 0
static void cb_flush (GtkAction *action)
{
  GeglNode *node;
  g_object_get (GEGL_VIEW(editor.view), "node", &node, NULL);
  gegl_buffer_flush (GEGL_BUFFER (gegl_node_get_cache (node)));

}
#endif

void editor_refresh_structure (void)
{
  GeglStore *store = gegl_store_new ();
  GtkWidget *treeview;

  gegl_store_set_gegl (store, editor.gegl);
  treeview = tree_editor_get_treeview (editor.tree_editor);
  gtk_tree_view_set_model (GTK_TREE_VIEW (treeview), NULL);
  gtk_tree_view_set_model (GTK_TREE_VIEW (treeview),
                           GTK_TREE_MODEL (store));
}

#if 1
gpointer gegl_node_get_pad (gpointer, const gchar *name);
gpointer gegl_pad_set_format (gpointer, gpointer);
#endif

static void editor_set_gegl (GeglNode    *gegl)
{
/*
  if (editor.options->file)
    g_free (editor.options->file);
  editor.options->file = g_strdup (path);*/

  if (editor.gegl)
    g_object_unref (editor.gegl);
  editor.gegl = gegl;

#if 1
    {
      GeglPad *pad;
      pad = gegl_node_get_pad (gegl, "output");
      g_assert (pad);
      gegl_pad_set_format (pad, babl_format ("R'G'B' u8"));  /* optimizes the cache used */
    }
#endif

  g_object_set (editor.view, "node", editor.gegl, NULL);
  editor_refresh_structure ();
}

void
gegl_gui_flush (void)
{
  gegl_view_repaint (GEGL_VIEW (editor.view));
}


typedef struct Command {
  gchar *command;
  gint (*callback) (gint argc, gchar **argv);
} Command;

static GList *commands = NULL;

static gint help (gint argc, gchar **argv)
{
  GList *iter;
  g_print ("Available commands:\n  ");
  for (iter=commands;iter;iter=iter->next)
    {
      Command *c = iter->data;
      g_print ("%s ", c->command);
    }
  g_print ("\n");
  return 0;
}

static gint
do_command_argv (gint argc, gchar **argv)
{
  GList *iter;
  if (commands == NULL)
    {
      #define o(cmd, cb) do{Command *c=g_new0(Command, 1);\
        c->command=g_strdup(cmd);\
        c->callback=(void*)(cb);\
        commands = g_list_append (commands, c);\
      }while(0)
      #include "editor-actions.inc"
      #undef o
    }
  for (iter=commands;iter;iter=iter->next)
    {
      Command *c = iter->data;
      if (g_str_equal (c->command, argv[0]))
        {
          return c->callback (argc, argv);
        }
    }
  g_print ("unknown command %s\n", argv[0]);
  return help(0, NULL);
}

static gint
do_command (const gchar *command)
{
  gint    argc;
  gint    retval;
  gchar **argv = NULL;

  if (command[0] == '\0')
    return FALSE;

  g_shell_parse_argv (command, &argc, &argv, NULL);
  retval = do_command_argv (argc, argv);
  g_strfreev (argv);
  return retval;
}
