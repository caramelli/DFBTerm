/*
   This file is part of DFBTerm.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
*/

#include <config.h>
#include <direct/thread.h>
#include <directfb.h>
#include <libzvt/vtx.h>
#include <lite/lite.h>
#include <lite/window.h>
#include <pwd.h>

/**********************************************************************************************************************/

#define TERM_FONT    "Misc-Fixed"
#define TERM_BGALPHA  0xe0
#define TERM_LINES    4000

#define TERM_DEFAULT_FONTSIZE  13
#define TERM_DEFAULT_COLS     100
#define TERM_DEFAULT_ROWS      30

typedef struct {
     IDirectFBFont              *font;
     int                         CW, CH;
     int                         width, height;
     LiteWindow                 *window;
     IDirectFBSurface           *surface;
     IDirectFBSurface           *bar_surface;
     int                         bar_start, bar_end;

     struct _vtx                *vtx;

     DirectThread               *update_thread;
     bool                        update_closing;
     DirectMutex                 lock;

     int                         cursor_state;

     DFBRegion                   flip_region;
     DFBBoolean                  flip_pending;

     DFBBoolean                  in_resize;

     struct timeval              last_click;

     DFBInputDeviceModifierMask  modifiers;
} Term;

/* The first 16 values are the ANSI colors, the last two are the default foreground and default background */

static const u8 default_red[] = {
     0x00, 0xaa, 0x00, 0xaa, 0x00, 0xaa, 0x00, 0xbb, 0x77, 0xff, 0x55, 0xff, 0x55, 0xff, 0x55, 0xff, 0xb0, 0x00
};

static const u8 default_grn[] = {
     0x00, 0x00, 0xaa, 0x55, 0x00, 0x00, 0xaa, 0xbb, 0x77, 0x55, 0xff, 0xff, 0x55, 0x55, 0xff, 0xff, 0xb0, 0x00
};

static const u8 default_blu[] = {
     0x00, 0x00, 0x00, 0x00, 0xaa, 0xaa, 0xaa, 0xbb, 0x77, 0x55, 0x55, 0x55, 0xff, 0xff, 0xff, 0xff, 0xb0, 0x00
};

/**********************************************************************************************************************/

static void add_flip( Term *term, DFBRegion *region )
{
     if (term->flip_pending) {
          if (term->flip_region.x1 > region->x1)
               term->flip_region.x1 = region->x1;

          if (term->flip_region.y1 > region->y1)
               term->flip_region.y1 = region->y1;

          if (term->flip_region.x2 < region->x2)
               term->flip_region.x2 = region->x2;

          if (term->flip_region.y2 < region->y2)
               term->flip_region.y2 = region->y2;
     }
     else {
          term->flip_region  = *region;
          term->flip_pending = DFB_TRUE;
     }
}

static int unichar_to_utf8( unsigned int c, char *s )
{
     int len = 0;
     int first;
     int i;

     if (c < 0x80) {
          first = 0;
          len = 1;
     }
     else if (c < 0x800) {
          first = 0xc0;
          len = 2;
     }
     else if (c < 0x10000) {
          first = 0xe0;
          len = 3;
     }
     else if (c < 0x200000) {
          first = 0xf0;
          len = 4;
     }
     else if (c < 0x4000000) {
          first = 0xf8;
          len = 5;
     }
     else {
          first = 0xfc;
          len = 6;
     }

     if (s) {
          for (i = len - 1; i > 0; --i) {
               s[i] = (c & 0x3f) | 0x80;
               c >>= 6;
          }

          s[0] = c | first;
     }

     return len;
}

static void vt_draw_text( void *user_data, struct vt_line *line, int row, int col, int len, int attr )
{
     DFBRegion  region;
     int        i, n, x, y, fore, back, fga, bga;
     char       text[len*6]; /* enough memory space for UTF-8 worst case */
     Term      *term = user_data;

     fore = (attr & VTATTR_FORECOLOURM) >> VTATTR_FORECOLOURB;
     back = (attr & VTATTR_BACKCOLOURM) >> VTATTR_BACKCOLOURB;

     if ((attr & VTATTR_BOLD) && fore < 8)
          fore |= 8;

     if (attr & VTATTR_REVERSE) {
          i    = fore;
          fore = back;
          back = i;

          fga  = TERM_BGALPHA;
          bga  = 0xff;
     }
     else {
          fga  = 0xff;
          bga  = TERM_BGALPHA;
     }

     x = col * term->CW;
     y = row * term->CH;

     region.x1 = x;
     region.y1 = y;
     region.x2 = x + len * term->CW - 1;
     region.y2 = y + term->CH - 1;

     term->surface->SetColor( term->surface, default_red[back], default_grn[back], default_blu[back], bga );

     term->surface->FillRectangle( term->surface, x, y, term->CW * len, term->CH );

     term->surface->SetColor( term->surface, default_red[fore], default_grn[fore], default_blu[fore], fga );

     for (i = 0, n = 0; i < len; i++) {
          unsigned int c;

          c = VT_ASCII( line->data[i+col] );

          if (c < 128)
               text[n++] = c;
          else
               n += unichar_to_utf8( c, text + n );
     }

     term->surface->DrawString( term->surface, text, n, x, y, DSTF_TOPLEFT );

     if (!term->in_resize)
          add_flip( term, &region );
}

static void vt_scroll_area( void *user_data, int firstrow, int count, int offset, int fill )
{
     DFBRegion     region;
     DFBRectangle  rect;
     Term         *term = user_data;

     rect.x = 0;
     rect.y = (firstrow + offset) * term->CH;
     rect.w = term->width;
     rect.h = count * term->CH;

     term->surface->Blit( term->surface, term->surface, &rect, 0, firstrow * term->CH );

     region.x1 = 0;
     region.x2 = term->width - 1;
     region.y1 = firstrow * term->CH;
     region.y2 = region.y1 + count * term->CH - 1;

     if (!term->in_resize)
          add_flip( term, &region );
}

static int vt_cursor_state( void *user_data, int state )
{
     Term        *term = user_data;
     struct _vtx *vtx  = term->vtx;

     /* Only call vt_draw_cursor() if the state has changed */
     if (term->cursor_state ^ state) {
          vt_draw_cursor( vtx, state );
          term->cursor_state = state;
     }

     return term->cursor_state;
}

/**********************************************************************************************************************/

static void term_update_scrollbar( Term *term )
{
     int          termrows, start, end, total;
     struct _vtx *vtx = term->vtx;

     if (!term->bar_surface)
          return;

     termrows = term->height / term->CH;

     total = vtx->vt.scrollbacklines + termrows;
     start = (vtx->vt.scrollbacklines + vtx->vt.scrollbackoffset) * term->height / total;
     end   = (vtx->vt.scrollbacklines + vtx->vt.scrollbackoffset + termrows) * term->height / total;

     if (!term->in_resize && start == term->bar_start && end == term->bar_end)
          return;

     if (start) {
          term->bar_surface->SetColor( term->bar_surface, 0x00, 0x00, 0x00, TERM_BGALPHA );
          term->bar_surface->FillRectangle( term->bar_surface, 0, 0, 2, start );
     }

     if (end > start) {
          term->bar_surface->SetColor( term->bar_surface, 0x80, 0x80, 0x80, TERM_BGALPHA );
          term->bar_surface->FillRectangle( term->bar_surface, 0, start, 2, end - start );
     }

     if (end < term->height) {
          term->bar_surface->SetColor( term->bar_surface, 0x00, 0x00, 0x00, TERM_BGALPHA );
          term->bar_surface->FillRectangle( term->bar_surface, 0, end, 2, term->height - end );
     }

     if (!term->in_resize)
          term->bar_surface->Flip( term->bar_surface, NULL,
                                   getenv( "LITE_WINDOW_DOUBLEBUFFER" ) ? DSFLIP_BLIT : DSFLIP_NONE );

     term->bar_start = start;
     term->bar_end   = end;
}

static void term_handle_button( Term *term, DFBWindowEvent *evt )
{
     int          button;
     int          down;
     long long    diff;
     int          qual = 0;
     struct _vtx *vtx  = term->vtx;

     switch (evt->button) {
          case DIBI_LEFT:
               button = 1;
               break;
          case DIBI_MIDDLE:
               button = 2;
               break;
          case DIBI_RIGHT:
               button = 3;
               break;
          default:
               return;
     }

     down = (evt->type == DWET_BUTTONDOWN);

     if (term->modifiers & DIMM_SHIFT)
          qual |= 1;
     if (term->modifiers & DIMM_CONTROL)
          qual |= 4;
     if (term->modifiers & DIMM_ALT)
          qual |= 8;

     if (!getenv( "LITE_NO_FRAME" )) {
          evt->x -= 5;
          evt->y -= 23;
     }

     evt->x /= term->CW;
     evt->y /= term->CH;

     if (vtx->selectiontype == VT_SELTYPE_NONE) {
          if (!(evt->modifiers & DIMM_SHIFT) && vt_report_button( &vtx->vt, down, button, qual, evt->x, evt->y ))
               return;
     }

     evt->y += vtx->vt.scrollbackoffset;

     if (down) {
          if (vtx->selected) {
               vtx->selstartx = vtx->selendx;
               vtx->selstarty = vtx->selendy;
               vt_draw_selection( vtx );
               vtx->selected = 0;
          }

          switch (evt->button) {
               case DIBI_LEFT:
                    term->window->window->GrabPointer( term->window->window );

                    diff = (evt->timestamp.tv_sec - term->last_click.tv_sec) * 1000000 +
                           (evt->timestamp.tv_usec - term->last_click.tv_usec);

                    if ((evt->modifiers & DIMM_CONTROL) || diff < 400000)
                         vtx->selectiontype = VT_SELTYPE_WORD | VT_SELTYPE_MOVED;
                    else
                         vtx->selectiontype = VT_SELTYPE_CHAR;

                    vtx->selectiontype |= VT_SELTYPE_BYSTART;

                    vtx->selstartx = evt->x;
                    vtx->selstarty = evt->y;
                    vtx->selendx   = evt->x;
                    vtx->selendy   = evt->y;

                    if (!vtx->selected) {
                         vtx->selstartxold = evt->x;
                         vtx->selstartyold = evt->y;
                         vtx->selendxold   = evt->x;
                         vtx->selendyold   = evt->y;
                         vtx->selected = 1;
                    }

                    vt_fix_selection( vtx );
                    vt_draw_selection( vtx );

                    term->last_click = evt->timestamp;
                    break;

               case DIBI_MIDDLE:
               case DIBI_RIGHT: {
                         unsigned int  size;
                         char         *mime_type;
                         void         *clip_data;
                         IDirectFB    *dfb = lite_get_dfb_interface();

                         if (dfb->GetClipboardData( dfb, &mime_type, &clip_data, &size ))
                              break;

                         if (!strcmp( mime_type, "text/plain" )) {
                              unsigned int  i;
                              char         *buffer = clip_data;

                              for (i = 0; i < size; i++)
                                   if (buffer[i] == '\n')
                                        buffer[i] = '\r';

                              vt_writechild( &vtx->vt, buffer, size );

                              if (vtx->vt.scrollbackoffset) {
                                   vtx->vt.scrollbackoffset = 0;

                                   vt_update( vtx, UPDATE_SCROLLBACK );

                                   vt_cursor_state( term, 1 );

                                   term_update_scrollbar( term );
                              }
                         }

                         D_FREE( clip_data );
                         D_FREE( mime_type );
                         break;
                    }

               default:
                    break;
          }
     }
     else {
          if (vtx->selectiontype & VT_SELTYPE_BYSTART) {
               vtx->selendx = evt->x + 1;
               vtx->selendy = evt->y;
          }
          else {
               vtx->selstartx = evt->x;
               vtx->selstarty = evt->y;
          }

          if (vtx->selectiontype & VT_SELTYPE_MOVED) {
               int        size;
               char      *clip_data;
               IDirectFB *dfb = lite_get_dfb_interface();

               vt_fix_selection( vtx );
               vt_draw_selection( vtx );

               clip_data = vt_get_selection( vtx, 1, &size );

               dfb->SetClipboardData( dfb, "text/plain", clip_data, size, NULL );
          }

          vtx->selectiontype = VT_SELTYPE_NONE;

          term->window->window->UngrabPointer( term->window->window );
     }
}

static void term_handle_motion( Term *term, DFBWindowEvent *evt )
{
     struct _vtx *vtx = term->vtx;

     if (!getenv( "LITE_NO_FRAME" )) {
          evt->x -= 5;
          evt->y -= 23;
     }

     evt->x /= term->CW;
     evt->y /= term->CH;

     if (vtx->selectiontype != VT_SELTYPE_NONE) {
          if (vtx->selectiontype & VT_SELTYPE_BYSTART) {
               vtx->selendx = evt->x + 1;
               vtx->selendy = evt->y + vtx->vt.scrollbackoffset;
          }
          else {
               vtx->selstartx = evt->x;
               vtx->selstarty = evt->y + vtx->vt.scrollbackoffset;
          }

          vtx->selectiontype |= VT_SELTYPE_MOVED;

          vt_fix_selection( vtx );
          vt_draw_selection( vtx );
     }
}

static void term_handle_key( Term *term, DFBWindowEvent *evt )
{
     struct _vtx *vtx = term->vtx;

     if (evt->modifiers == DIMM_CONTROL && evt->key_symbol >= DIKS_SMALL_A && evt->key_symbol <= DIKS_SMALL_Z) {
          char c = evt->key_symbol - DIKS_SMALL_A + 1;
          vt_writechild (&vtx->vt, &c, 1);
     }
     else if ((evt->key_symbol > 9 && evt->key_symbol < 127) || (evt->key_symbol > 127 && evt->key_symbol < 256)) {
          char c = evt->key_symbol;

          if (evt->modifiers & DIMM_CONTROL) {
               switch (evt->key_symbol) {
                    case ' ':
                         vt_writechild( &vtx->vt, "\000", 1 );
                         break;
                    case '3':
                    case '[':
                         vt_writechild( &vtx->vt, "\033", 1 );
                         break;
                    case '4':
                    case '\\':
                         vt_writechild( &vtx->vt, "\034", 1 );
                         break;
                    case '5':
                    case ']':
                         vt_writechild( &vtx->vt, "\035", 1 );
                         break;
                    case '6':
                         vt_writechild( &vtx->vt, "\036", 1 );
                         break;
                    case '7':
                    case '-':
                         vt_writechild( &vtx->vt, "\037", 1 );
                         break;
                    default:
                         vt_writechild( &vtx->vt, &c, 1 );
                         break;
               }
          }
          else
               vt_writechild( &vtx->vt, &c, 1 );
     }
     else {
          switch (evt->key_symbol) {
               case DIKS_BACKSPACE:
                    vt_writechild( &vtx->vt, "\177", 1 );
                    break;
               case DIKS_TAB:
                    if (evt->modifiers & DIMM_SHIFT)
                         vt_writechild( &vtx->vt, "\033[Z", 3 );
                    else
                         vt_writechild( &vtx->vt, "\t", 1 );
                    break;
               case DIKS_DELETE:
                    vt_writechild( &vtx->vt, "\033[3~", 4 );
                    break;
               case DIKS_INSERT:
                    vt_writechild( &vtx->vt, "\033[2~", 4 );
                    break;
               case DIKS_CURSOR_LEFT:
                    vt_writechild( &vtx->vt, "\033[D", 3 );
                    break;
               case DIKS_CURSOR_RIGHT:
                    vt_writechild( &vtx->vt, "\033[C", 3 );
                    break;
               case DIKS_CURSOR_UP:
                    vt_writechild( &vtx->vt, "\033[A", 3 );
                    break;
               case DIKS_CURSOR_DOWN:
                    vt_writechild( &vtx->vt, "\033[B", 3 );
                    break;
               case DIKS_HOME:
                    vt_writechild( &vtx->vt, "\033OH", 3 );
                    break;
               case DIKS_END:
                    vt_writechild( &vtx->vt, "\033OF", 3 );
                    break;
               case DIKS_PAGE_UP:
                    vt_writechild( &vtx->vt, "\033[5~", 4 );
                    break;
               case DIKS_PAGE_DOWN:
                    vt_writechild( &vtx->vt, "\033[6~", 4 );
                    break;
               case DIKS_F1:
                    vt_writechild( &vtx->vt, "\033OP", 3 );
                    break;
               case DIKS_F2:
                    vt_writechild( &vtx->vt, "\033OQ", 3 );
                    break;
               case DIKS_F3:
                    vt_writechild( &vtx->vt, "\033OR", 3 );
                    break;
               case DIKS_F4:
                    vt_writechild( &vtx->vt, "\033OS", 3 );
                    break;
               case DIKS_F5 ... DIKS_F12: {
                         char                buf[6];
                         const unsigned char f5_f12_remap[] = { 15, 17, 18, 19, 20, 21, 23,24 };

                         sprintf( buf, "\033[%d~", f5_f12_remap[evt->key_symbol-DIKS_F5] );
                         vt_writechild( &vtx->vt, buf, strlen( buf ) );
                    }
                    break;
               default:
                    return;
          }
     }

     if (vtx->selected) {
          vtx->selstartx = vtx->selendx;
          vtx->selstarty = vtx->selendy;
          vt_draw_selection( vtx );
          vtx->selected = 0;
     }

     if (vtx->vt.scrollbackoffset) {
          vtx->vt.scrollbackoffset = 0;

          vt_update( vtx, UPDATE_SCROLLBACK );

          vt_cursor_state( term, 1 );

          term_update_scrollbar( term );
     }
}

static void term_handle_wheel( Term *term, DFBWindowEvent *evt )
{
     int          i;
     struct _vtx *vtx = term->vtx;

     if (evt->step > 0) {
          for (i = 0; i < evt->step; i++)
               vt_writechild( &vtx->vt, "\033[A", 3 );
     }
     else {
          for (i = 0; i > evt->step; i--)
               vt_writechild( &vtx->vt, "\033[B", 3 );
     }
}

static void term_scroll( Term *term, int scroll )
{
     struct _vtx *vtx = term->vtx;

     vtx->vt.scrollbackoffset += scroll;

     if (vtx->vt.scrollbackoffset > 0)
          vtx->vt.scrollbackoffset = 0;
     else if (vtx->vt.scrollbackoffset < -vtx->vt.scrollbacklines)
          vtx->vt.scrollbackoffset = -vtx->vt.scrollbacklines;

     vt_update( vtx, UPDATE_SCROLLBACK );

     vt_cursor_state( term, 1 );

     term_update_scrollbar( term );
}

static void term_flush_flip( Term *term )
{
     if (term->flip_pending) {
          term->surface->Flip( term->surface, &term->flip_region,
                               getenv( "LITE_WINDOW_DOUBLEBUFFER" ) ? DSFLIP_BLIT: DSFLIP_NONE );

          term->flip_pending = DFB_FALSE;
     }
}

/**********************************************************************************************************************/

static void *term_update( DirectThread *thread, void *arg )
{
     Term        *term = arg;
     struct _vtx *vtx = term->vtx;

     while (1) {
          int            count, update = 0;
          char           buffer[4096];
          fd_set         set;
          struct timeval tv;

          FD_ZERO( &set );
          FD_SET( vtx->vt.childfd, &set );
          FD_SET( vtx->vt.msgfd, &set );

          tv.tv_sec  = 10;
          tv.tv_usec = 0;

          if (select( MAX( vtx->vt.childfd, vtx->vt.msgfd ) + 1, &set, NULL, NULL, &tv ) < 0) {
               D_PERROR( "select() failed!\n" );

               if (errno == EINTR)
                    continue;

               break;
          }

          if (FD_ISSET( vtx->vt.msgfd, &set )) {
               term->update_closing = true;
               break;
          }

          while ((count = read( vtx->vt.childfd, buffer, sizeof(buffer) )) > 0) {
               vt_cursor_state( term, 0 );

               update = 1;

               vt_parse_vt( &vtx->vt, buffer, count );
          }

          if (update) {
               direct_mutex_lock( &term->lock );

               vt_update( vtx, UPDATE_CHANGES );

               vt_cursor_state( term, 1 );

               term_update_scrollbar( term );

               term_flush_flip( term );

               direct_mutex_unlock( &term->lock );
          }
     }

     return NULL;
}

static int on_window_resize( LiteWindow *window, int width, int height )
{
     DFBRectangle  rect;
     int           termcols, termrows;
     Term         *term    = LITE_BOX(window)->user_data;;
     struct _vtx  *vtx = term->vtx;

     term->in_resize = DFB_TRUE;

     if (term->bar_surface)
          term->bar_surface->Release( term->bar_surface );

     term->surface->Release( term->surface );

     term->width  = width - 2;
     term->height = height;

     termcols = term->width  / term->CW;
     termrows = term->height / term->CH;

     /* Initialize sub area for terminal */
     rect.x = 0; rect.y = 0; rect.w = term->width; rect.h = term->height;
     window->box.surface->GetSubSurface( window->box.surface, &rect, &term->surface );

     term->surface->Clear( term->surface, default_red[17], default_grn[17], default_blu[17], TERM_BGALPHA );

     term->surface->SetFont( term->surface, term->font );

     /* Initialize sub area for scroll bar */
     rect.x = term->width; rect.w = 2;
     window->box.surface->GetSubSurface( window->box.surface, &rect, &term->bar_surface );

     /* VTX */
     vt_resize( &vtx->vt, termcols, termrows, term->width, term->height );

     vt_update_rect( vtx, 1, 0, 0, termcols, termrows );

     vt_cursor_state( term, 1 );

     term_update_scrollbar( term );

     term->in_resize = DFB_FALSE;

     term->flip_pending = DFB_FALSE;

     return 1;
}

static void term_usage()
{
     printf( "DirectFB Terminal Emulator\n\n" );
     printf( "Usage: dfbterm [options]\n\n" );
     printf( "Options:\n\n" );
     printf( "  --fontsize=<size>     Set font size (default = %d).\n", TERM_DEFAULT_FONTSIZE );
     printf( "  --size=<cols>x<rows>  Set terminal size (default = %dx%d).\n", TERM_DEFAULT_COLS, TERM_DEFAULT_ROWS );
     printf( "  --position=<x,y>      Set terminal position.\n" );
     printf( "  --help                Print usage information.\n" );
}

int main( int argc, char *argv[] )
{
     DFBResult             ret      = DFB_FAILURE;
     int                   quit     = 0;
     char                 *geosep   = NULL;
     char                 *geometry = NULL;
     int                   fontsize = TERM_DEFAULT_FONTSIZE;
     int                   termcols = TERM_DEFAULT_COLS;
     int                   termrows = TERM_DEFAULT_ROWS;
     int                   termposx = -666;
     int                   termposy = -666;
     int                   len      = strlen( TERMFONTDIR ) + 1 + strlen( TERM_FONT ) + 6 + 1;
     char                  filename[len];
     DFBFontDescription    desc;
     DFBRectangle          rect;
     int                   i;
     Term                 *term;
     struct _vtx          *vtx;
     IDirectFBEventBuffer *event_buffer;

     /* Parse command line */
     for (i = 1; i < argc; i++) {
          if (!strcmp( argv[i], "--help" )) {
               term_usage();
               return 0;
          }
          else if (strstr( argv[i], "--fontsize=" ) == argv[i]) {
               fontsize = atoi( 1 + index( argv[i], '=' ) );
               if (fontsize < 2 || fontsize > 666) {
                    DirectFBError( "Bad fontsize", DFB_FAILURE );
                    return 1;
               }
          }
          else if (strstr( argv[i], "--size=" ) == argv[i]) {
               geometry = 1 + index( argv[i], '=' );

               geosep = index( geometry, 'x' );

               if (!geosep) {
                    DirectFBError( "Bad size format", DFB_FAILURE );
                    term_usage();
                    return 1;
               }

               termrows = atoi( geosep + 1 );
               if (termrows < 1 || termrows > 666) {
                    DirectFBError( "Invalid number of rows", DFB_FAILURE );
                    return 1;
               }
               else
                    termrows = (termrows == 666) ? TERM_DEFAULT_ROWS : termrows;

               *geosep = 0;

               termcols = atoi( geometry );
               if (termcols < 1 || termcols > 666) {
                    DirectFBError( "Invalid number of cols", DFB_FAILURE );
                    return 1;
               }
               else
                    termcols = (termcols == 666) ? TERM_DEFAULT_COLS : termcols;
          }
          else if (strstr( argv[i], "--position=" ) == argv[i]) {
               geometry = 1 + index( argv[i], '=' );

               geosep = index( geometry, ',' );

               if (!geosep) {
                    DirectFBError( "Bad position format", DFB_FAILURE );
                    term_usage();
                    return 1;
               }

               termposy = atoi( geosep + 1 );

               *geosep = 0;

               termposx = atoi( geometry );
          }
     }

     /* Initialize */
     setenv( "LITE_NO_CURSOR", "1", 1 );

     if (lite_open( &argc, &argv ))
          return 1;

     unsetenv( "LITE_NO_CURSOR" );

     term = D_CALLOC( 1, sizeof(Term) );

     /* Load terminal font */
     desc.flags  = DFDESC_HEIGHT;
     desc.height = fontsize;

     if (!getenv( "LITE_NO_DGIFF" )) {
          IDirectFB *dfb = lite_get_dfb_interface();

          /* First try to load terminal font in DGIFF format */
          snprintf( filename, len, TERMFONTDIR"/%s.dgiff", TERM_FONT );
          ret = dfb->CreateFont( dfb, filename, &desc, &term->font );
          if (ret)
               DirectFBError( "CreateFont() failed", ret );
     }

     if (ret) {
          IDirectFB *dfb = lite_get_dfb_interface();

          /* Otherwise fall back on terminal font in TTF format */
          snprintf( filename, len, TERMFONTDIR"/%s.ttf", TERM_FONT );
          ret = dfb->CreateFont( dfb, filename, &desc, &term->font );
          if (ret) {
               DirectFBError( "CreateFont() failed", ret );
               D_FREE( term );
               lite_close();
               return 1;
          }
     }

     /* Calculate terminal size */
     term->font->GetGlyphExtents( term->font, 'O', NULL, &term->CW );
     term->font->GetHeight( term->font, &term->CH );

     term->width  = term->CW * termcols;
     term->height = term->CH * termrows;

     /* Create terminal window with scroll bar */
     rect.x = LITE_CENTER_HORIZONTALLY; rect.y = LITE_CENTER_VERTICALLY; rect.w = term->width + 2; rect.h = term->height;
     ret = lite_new_window( NULL, &rect, DWCAPS_ALPHACHANNEL, liteDefaultWindowTheme, "DFBTerm", &term->window );
     if (ret) {
          D_FREE( term );
          lite_close();
          return 1;
     }

     /* Setup the terminal window */
     term->window->step_x = term->CW;
     term->window->step_y = term->CH;

     lite_set_window_background( term->window, NULL );
     lite_set_window_blend_mode( term->window, LITE_BLEND_AUTO, LITE_BLEND_AUTO );

     lite_draw_box( LITE_BOX(term->window), NULL, DFB_TRUE );

     if (termposx != -666 || termposy != -666) {
          int x, y;

          term->window->window->GetPosition( term->window->window, &x, &y );

          x = (termposx != -666) ? termposx : x;
          y = (termposy != -666) ? termposy : y;

          term->window->window->MoveTo( term->window->window, x, y );
     }

     /* Install callback */
     LITE_BOX(term->window)->user_data = term;
     term->window->OnResize = on_window_resize;

     /* Initialize sub area for terminal */
     rect.x = 0; rect.y = 0; rect.w = term->width; rect.h = term->height;
     term->window->box.surface->GetSubSurface( term->window->box.surface, &rect, &term->surface );

     term->surface->Clear( term->surface, default_red[17], default_grn[17], default_blu[17], TERM_BGALPHA );

     term->surface->SetFont( term->surface, term->font );

     /* Initialize sub area for scroll bar */
     if (!getenv( "DFBTERM_NO_SCROLLBAR" )) {
          rect.x = term->width; rect.w = 2;
          term->window->box.surface->GetSubSurface( term->window->box.surface, &rect, &term->bar_surface );
     }

     term->bar_start = -1;
     term->bar_end   = -1;

     /* Create event buffer */
     lite_get_event_buffer( &event_buffer );

     /* VTX */
     vtx = vtx_new( termcols, termrows, term );
     if (!vtx) {
          DirectFBError( "Failed to create VTX object", DFB_FAILURE );
          if (term->bar_surface)
               term->bar_surface->Release( term->bar_surface );
          term->surface->Release( term->surface );
          term->font->Release( term->font );
          D_FREE( term );
          lite_close();
     }
     else
          term->vtx = vtx;

     vtx->draw_text    = vt_draw_text;
     vtx->scroll_area  = vt_scroll_area;
     vtx->cursor_state = vt_cursor_state;
     vtx->scroll_type  = VT_SCROLL_SOMETIMES;

     if ((i = vt_forkpty( &vtx->vt, 0 )) == 0) {
          char          *shell, *name;
          struct passwd *pw;

          pw = getpwuid( getuid() );
          if (pw) {
               shell = pw->pw_shell;
               name  = strrchr( shell, '/' ) + 1;
          }
          else {
               shell = "/bin/sh";
               name  = "sh";
          }

          setenv( "TERM", "linux", 1 );

          execl( shell, name, "-", NULL );

          D_PERROR( "execl() failed!\n" );

          _exit( 127 );
     }
     else if (i == -1) {
          DirectFBError( "Failed to start child process", DFB_FAILURE );
          vtx_destroy( vtx );
          if (term->bar_surface)
               term->bar_surface->Release( term->bar_surface );
          term->surface->Release( term->surface );
          term->font->Release( term->font );
          D_FREE( term );
          lite_close();
          return 1;
     }

     direct_mutex_init( &term->lock );

     direct_mutex_lock( &term->lock );

     term->update_thread = direct_thread_create( DTT_DEFAULT, term_update, term, "Term Update" );

     vt_cursor_state( term, 1 );

     vt_scrollback_set( &vtx->vt, TERM_LINES );

     /* Show the terminal window */
     lite_set_window_opacity( term->window, liteFullWindowOpacity );

     term->window->window->RequestFocus( term->window->window );

     /* Loop */
     while (!quit && !term->update_closing) {
          DFBWindowEvent evt;
          int            scroll = 0;

          direct_mutex_unlock( &term->lock );

          event_buffer->WaitForEvent( event_buffer );

          direct_mutex_lock( &term->lock );

          while (event_buffer->GetEvent( event_buffer, DFB_EVENT(&evt) ) == DFB_OK) {
               if (lite_handle_window_event( term->window, &evt) )
                    continue;

               switch (evt.type) {
                    case DWET_BUTTONDOWN:
                    case DWET_BUTTONUP:
                         term_handle_button( term, &evt );
                         break;
                    case DWET_MOTION:
                         term_handle_motion( term, &evt );
                         break;
                    case DWET_KEYDOWN:
                         term->modifiers = evt.modifiers;
                         if (evt.modifiers & DIMM_SHIFT) {
                              switch (evt.key_symbol) {
                                   case DIKS_CURSOR_UP:
                                        scroll -= 1;
                                        break;
                                   case DIKS_CURSOR_DOWN:
                                        scroll += 1;
                                        break;
                                   case DIKS_PAGE_UP:
                                        scroll -= termrows - 1;
                                        break;
                                   case DIKS_PAGE_DOWN:
                                        scroll += termrows - 1;
                                        break;
                                   default:
                                        term_handle_key( term, &evt );
                                        break;
                              }
                         }
                         else
                              term_handle_key( term, &evt );
                         break;
                    case DWET_KEYUP:
                         term->modifiers = evt.modifiers;
                         break;
                    case DWET_WHEEL:
                         if (evt.modifiers & DIMM_CONTROL)
                              term_handle_wheel( term, &evt );
                         else if (evt.modifiers & DIMM_SHIFT)
                              scroll -= evt.step;
                         else
                              scroll -= (termrows - 1) * evt.step;
                         break;
                    case DWET_CLOSE:
                    case DWET_DESTROYED:
                         quit = 1;
                         break;
                    default:
                         break;
               }
          }

          lite_flush_window_events( term->window );

          if (scroll)
               term_scroll( term, scroll );

          term_flush_flip( term );
     }

     if (!term->update_closing)
          direct_thread_cancel( term->update_thread );
     direct_thread_join( term->update_thread );
     direct_thread_destroy( term->update_thread );

     direct_mutex_deinit( &term->lock );

     vt_closepty( &vtx->vt );
     vtx_destroy( vtx );

     if (term->bar_surface)
          term->bar_surface->Release( term->bar_surface );
     term->surface->Release( term->surface );
     term->font->Release( term->font );

     D_FREE( term );

     lite_close();

     return 0;
}
