/* WindowLab - an X11 window manager
 * Copyright (c) 2001-2010 Nick Gravgaard
 * me at nickgravgaard.com
 * http://nickgravgaard.com/windowlab/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "windowlab.h"

static void init_position(Client *);
static void reparent(Client *);
#ifdef MWM_HINTS
static Bool get_mwm_hints(Window, PropMwmHints *);
#endif

/* Set up a client structure for the new (not-yet-mapped) window. The
 * confusing bit is that we have to ignore 2 unmap events if the
 * client was already mapped but has IconicState set (for instance,
 * when we are the second window manager in a session). That's
 * because there's one for the reparent (which happens on all viewable
 * windows) and then another for the unmapping itself. */

void make_new_client(Window w)
{
	Client *c, *p;
	XWindowAttributes attr;
	XWMHints *hints;
#ifdef MWM_HINTS
	PropMwmHints mhints;
#endif
	long dummy;

	c = (Client *)malloc(sizeof *c);
	if (head_client == NULL)
	{
		head_client = c;
	}
	else
	{
		p = head_client;
		while (p->next != NULL)
		{
			p = p->next;
		}
		p->next = c;
	}
	c->next = NULL;

	XGrabServer(dsply);

	XGetTransientForHint(dsply, w, &c->trans);
	XGetWindowAttributes(dsply, w, &attr);

	c->window = w;
	c->ignore_unmap = 0;
	c->hidden = 0;
	c->was_hidden = 0;
#ifdef SHAPE
	c->has_been_shaped = 0;
#endif
	c->x = attr.x;
	c->y = attr.y;
	c->width = attr.width;
	c->height = attr.height;
	c->cmap = attr.colormap;
	c->size = XAllocSizeHints();
	XGetWMNormalHints(dsply, c->window, c->size, &dummy);
	update_title(c);
#ifdef MWM_HINTS
	c->has_title = 1;
	c->has_border = 1;

	if (get_mwm_hints(c->window, &mhints))
	{
		if (mhints.flags & MWM_HINTS_DECORATIONS && !(mhints.decorations & MWM_DECOR_ALL))
		{
			c->has_title = mhints.decorations & MWM_DECOR_TITLE;
			c->has_border = mhints.decorations & MWM_DECOR_BORDER;
		}
	}
#endif

	// XReparentWindow seems to try an XUnmapWindow, regardless of whether the reparented window is mapped or not
	c->ignore_unmap++;
	
	if (attr.map_state != IsViewable)
	{
		init_position(c);
		set_wm_state(c, NormalState);
		if ((hints = XGetWMHints(dsply, w)))
		{
			if (hints->flags & StateHint)
			{
				set_wm_state(c, hints->initial_state);
			}
			XFree(hints);
		}
	}

	fix_position(c);
	gravitate(c, APPLY_GRAVITY);
	reparent(c);

#ifdef XFT
	c->xftdraw = XftDrawCreate(dsply, (Drawable) c->frame, DefaultVisual(dsply, DefaultScreen(dsply)), DefaultColormap(dsply, DefaultScreen(dsply)));
#endif

	if (get_wm_state(c) != IconicState)
	{
		XMapWindow(dsply, c->window);
		XMapRaised(dsply, c->frame);

		topmost_client = c;
	}
	else
	{
		c->hidden = 1;
		if(attr.map_state == IsViewable)
		{
			c->ignore_unmap++;
			XUnmapWindow(dsply, c->window);
		}
	}

	// // if no client has focus give focus to the new client
	check_focus(c);
	if (focused_client == NULL)
	{
		focused_client = c;
	}

	XSync(dsply, False);
	XUngrabServer(dsply);

	reorder_clients_by_x_position();
	redraw_taskbar();
}

#ifdef MWM_HINTS
static Bool get_mwm_hints(Window w, PropMwmHints *hints)
{
	Atom real_type;
	int real_format;
	unsigned long items_read, items_left;
	// NOTE: See XGetWindowProperty(3)
	// > If the returned format is 32, the property data will be stored as an array of longs (which in a 64-bit application will be
	// > 64-bit values that are padded in the upper 4 bytes).
	unsigned long *data = NULL;
	
	int success = XGetWindowProperty(dsply, w, mwm_hints, 0, PROP_MWM_HINTS_ELEMENTS, False, mwm_hints, &real_type, &real_format, &items_read, &items_left, (unsigned char **)&data);
	if (success != Success)
	{
		err("cannot read hints property for window 0x%x", w);
		return False;
	}
	if (real_type == None)
	{
		return False;
	}
	if (real_type != mwm_hints || real_format != 32)
	{
		err("bad hints property read for window 0x%x", w);
		return False;
	}
	if (items_read < PROP_MWM_HINTS_ELEMENTS)
	{
		err("hints property too small for window 0x%x (missing %d of %d elements)", w, (PROP_MWM_HINTS_ELEMENTS - items_read), PROP_MWM_HINTS_ELEMENTS);
		return False;
	}
	
	hints->flags = (CARD32)data[0];
	hints->functions = (CARD32)data[1];
	hints->decorations = (CARD32)data[2];
	hints->inputMode = (INT32)data[3];
	hints->status = (CARD32)data[4];
	XFree(data);
	return True;
}
#endif

/* Figure out where to map the window. c->x, c->y, c->width, and
 * c->height actually start out with values in them (whatever the
 * client passed to XCreateWindow).
 *
 * The ICCM says that there are no position/size fields anymore and
 * the SetWMNormalHints says that they are obsolete, so we use the values
 * we got from the window attributes
 * We honour both program and user preferences
 *
 * If we can't find a reasonable position hint, we make up a position
 * using the relative mouse co-ordinates and window size. To account
 * for window gravity while doing this, we add BARHEIGHT() into the
 * calculation and then degravitate. Don't think about it too hard, or
 * your head will explode. */

static void init_position(Client *c)
{
	int mousex, mousey;

	if (c->width < MINWINWIDTH)
	{
		c->width = MINWINWIDTH;
	}
	if (c->height < MINWINHEIGHT)
	{
		c->height = MINWINHEIGHT;
	}

	if (c->x == 0 && c->y == 0)
	{
		get_mouse_position(&mousex, &mousey);
		c->x = mousex;
		c->y = mousey + TITLEHEIGHT(c);
		gravitate(c, REMOVE_GRAVITY);
	}
}

static void reparent(Client *c)
{
	XSetWindowAttributes pattr;

	pattr.override_redirect = True;
	pattr.background_pixel = empty_col.pixel;
	pattr.border_pixel = border_col.pixel;
	pattr.event_mask = ChildMask|ButtonPressMask|ExposureMask|EnterWindowMask;
	c->frame = XCreateWindow(dsply, root, c->x, c->y - TITLEHEIGHT(c), c->width, c->height + TITLEHEIGHT(c), BORDERWIDTH(c), DefaultDepth(dsply, screen), CopyFromParent, DefaultVisual(dsply, screen), CWOverrideRedirect|CWBackPixel|CWBorderPixel|CWEventMask, &pattr);

#ifdef SHAPE
	if (shape)
	{
		XShapeSelectInput(dsply, c->window, ShapeNotifyMask);
		set_shape(c);
	}
#endif

	XAddToSaveSet(dsply, c->window);
	XSelectInput(dsply, c->window, ColormapChangeMask|PropertyChangeMask);
	XSetWindowBorderWidth(dsply, c->window, 0);
	XResizeWindow(dsply, c->window, c->width, c->height);
	XReparentWindow(dsply, c->window, c->frame, 0, TITLEHEIGHT(c));

	send_config(c);
}
