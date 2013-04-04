//#define _POSIX_C_SOURCE 199309L
//#define _BSD_SOURCE
#define LOCAL_ENABLE

#include "fontmap_static.h"
#include "widthmap_static.h"

#include <string.h>
#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include "vterm.h"
#include <locale.h>

#include "nunifont.h"
#include <limits.h>

#include "nsdl.h"
#include <time.h>
#include "regis.h"
#include <stdbool.h>
#include "ssh.h"
#include "local.h"
#include "inlinedata.h"
#include "ngui.h"
#include "iphone_pasteboard.h"
#include "utf8proc.h"


#define CONNECTION_LOCAL 1
#define CONNECTION_SSH   2


void redraw_required();

int font_width  = 8;
int font_height = 16;
int font_space  = 0;

static VTerm *vt;
static VTermScreen *vts;

bool any_blinking       = false;
bool new_screen_size    = false;
int  new_screen_size_x;
int  new_screen_size_y;

static int cols;
static int rows;

int display_width;
int display_height;
int display_width_last_kb=0;
int display_height_last_kb=0;
int display_width_abs;
int display_height_abs;

SDL_Window  *screen=1;
SDL_Renderer *renderer=1;

bool draw_selection = false;
int draw_fade_selection=0;
int select_start_x=0;
int select_start_y=0;
int select_end_x  =0;
int select_end_y  =0;
int select_start_scroll_offset;
int select_end_scroll_offset;
int connection_type=0;

bool hterm_quit = false;

int       scroll_offset=0;
size_t    scroll_buffer_initial_size = 10000;

size_t    scroll_buffer_size = 0;
size_t    scroll_buffer_start =0;
size_t    scroll_buffer_end   =0;
VTermScreenCell **scroll_buffer = 0;
uint32_t         *scroll_buffer_lens=0;

VTermState *vs;

char open_arg1[100] = "";
char open_arg2[100];
char open_arg3[100];
char open_arg4[100];
char open_arg5[100];
char open_arg6[100];

int select_text_start_x=-1;
int select_text_start_y=-1;
int select_text_end_x=-1;
int select_text_end_y=-1;

// Funtions used to communicate with host
int (*c_open)(char *hostname,char *username, char *password,char *fingerprintstr,char *key1,char *key2) = 0;
int (*c_close)() = 0;
int (*c_write)(char *bytes,int len) = 0;       
int (*c_read)(char *bytes,int len) = 0;    
int (*c_resize)(int rows,int cols) = 0;

void scroll_buffer_get(size_t line_number,VTermScreenCell **line,int *len);

bool hterm_next_key_ctrl=false;

void regis_render() {

  if(!regis_cleared()) {
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, regis_layer);
    
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_DestroyTexture(texture);
  }
}

void inline_data_render() {                                        

  if(inline_data_layer != 0) {
    printf("inline blit\n");
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, inline_data_layer);
    
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_DestroyTexture(texture);
    printf("inline blit'd\n");
  }
}


VTermScreenCell *grab_row(int trow,bool *dont_free,int *len) {

  VTermScreenCell *rowdata = 0;

  if(trow >= 0) {
    // a screen row
    rowdata = malloc(cols*sizeof(VTermScreenCell));
    VTermPos vp;
    for(int n=0;n<cols;n++) {
      vp.row = trow;
      vp.col = n;
      vterm_screen_get_cell(vts,vp,&(rowdata[n]));
    }
    *len = cols;
    *dont_free =false;
  } else {
    // a scrollback row
    if((0-trow) > scroll_buffer_size) { rowdata = 0; }
    else {
      scroll_buffer_get(0-trow,&rowdata,len);
      *dont_free=true;
    }
  }

  return rowdata;
}

VTermScreenCell c_screen_data[1000][1000];

bool cdf =true;

bool cellcompare(VTermScreenCell a,VTermScreenCell b) {
  if(a.chars[0] != b.chars[0]) return false;
  if(a.chars[1] != b.chars[1]) return false;
    
  if(a.attrs.bold != b.attrs.bold) return false;
  if(a.attrs.underline != b.attrs.underline) return false;
  if(a.attrs.italic != b.attrs.italic) return false;
  if(a.attrs.blink  != b.attrs.blink) return false;
  if(a.attrs.reverse != b.attrs.reverse) return false;
  if(a.attrs.strike != b.attrs.strike) return false;
  if(a.attrs.font != b.attrs.font) return false;
  
  return true;
}

void draw_row(VTermScreenCell *row,int crow,int ypos,int glen) {
/*
    if(cdf==true) {
        for(int n=0;n<1000;n++){
            for(int i=0;i<1000;i++) {
                c_screen_data[n][i].chars[0]=0;
            }
        }
    }
    cdf=false;
 */
  int xpos=0;

  for(int n=0;n<cols;n++) {
    if(n >= glen) break;
    uint16_t rtext[1000];

    rtext[0] = row[n].chars[0];
    if(rtext[0]==0) rtext[0]=' ';
    rtext[1]=0;

    VTermColor fg = row[n].fg;
    VTermColor bg = row[n].bg;

    //if(cellcompare(c_screen_data[crow][n],row[n]) == false) {
      if(row[n].attrs.blink == 1) any_blinking = true;
      draw_unitext_fancy_renderer(renderer,xpos,ypos,rtext,(bg.red << 24) + (bg.green << 16) + (bg.blue << 8) + 0xff,
                                                (fg.red << 24) + (fg.green << 16) + (fg.blue << 8) + 0xff,
                                                row[n].attrs.bold,
                                                row[n].attrs.underline,
                                                row[n].attrs.italic,
                                                row[n].attrs.blink,
                                                row[n].attrs.reverse,
                                                row[n].attrs.strike,
                                                row[n].attrs.font);
    //}
    //c_screen_data[crow][n] = row[n];
      
    xpos+=(font_width+font_space);
    if(row[n].width == 2) {xpos +=(font_width+font_space);n++;}
  }

}

void scroll_buffer_init() {
  scroll_buffer      = malloc(sizeof(VTermScreenCell *)*scroll_buffer_initial_size);
  scroll_buffer_lens = malloc(sizeof(int32_t)*scroll_buffer_initial_size);
  for(int n=0;n<scroll_buffer_initial_size;n++) {
    scroll_buffer[n] = 0;
    scroll_buffer_lens[n]=0;
  }
  
  scroll_buffer_size = scroll_buffer_initial_size;
  scroll_buffer_start=0;
  scroll_buffer_end  =0;
}

void scroll_buffer_push(VTermScreenCell *scroll_line,size_t len) {

   if(scroll_buffer == 0) scroll_buffer_init();

   if(scroll_buffer_end >= scroll_buffer_size) scroll_buffer_end = 0;

   if(scroll_buffer[scroll_buffer_end] != 0) {
     // if infini buffer, do resize
     // scroll_buffer_resize(scroll_buffer_size+10000);
     // else
     free(scroll_buffer[scroll_buffer_end]);
     scroll_buffer[scroll_buffer_end]=0;
   }

  scroll_buffer[scroll_buffer_end] = malloc(sizeof(VTermScreenCell)*len);
  scroll_buffer_lens[scroll_buffer_end] = len;

  for(size_t n=0;n<len;n++) {
    scroll_buffer[scroll_buffer_end][n] = scroll_line[n];
  }

  scroll_buffer_end++;
}

void scroll_buffer_get(size_t line_number,VTermScreenCell **line,int *len) {
  int idx = scroll_buffer_end-line_number-1;

  if(idx < 0) idx = scroll_buffer_size+idx;
  if(idx < 0) *line = 0;

  *line = scroll_buffer[idx];
  *len  = scroll_buffer_lens[idx];
  
}

void scroll_buffer_dump() {
}

static int screen_prescroll(VTermRect rect, void *user)
{
  if(rect.start_row != 0 || rect.start_col != 0 || rect.end_col != cols)
    return 0;

  
  for(int row=rect.start_row;row<rect.end_row;row++) {
    VTermScreenCell scrolloff[1000];

    size_t len=0;
    for(int n=0;n<cols;n++) {
      VTermPos vp;
      vp.row=row;
      vp.col=n;
      VTermScreenCell c;
      int i = vterm_screen_get_cell(vts,vp,&c);
      scrolloff[n] = c;
      len++;
    }
    scroll_buffer_push(scrolloff,cols);

  }
  redraw_required();
  return 1;
}

static int screen_resize(int new_rows, int new_cols, void *user)
{
  rows = new_rows;
  cols = new_cols;
  return 1;
}

static int parser_resize(int new_rows, int new_cols, void *user)
{
  return 1;
}

static int screen_bell(void* d) {

}

int state_erase(VTermRect r,void *user) {
  printf("********************************************** clear received\n");
  redraw_required();
  return 0;
}

VTermScreenCallbacks cb_screen = {
  .prescroll = &screen_prescroll,
  .resize    = &screen_resize,
  .bell      = &screen_bell
};

VTermStateCallbacks cb_state = {
  .putglyph     = 0,
  .movecursor   = 0,
  .scrollrect   = 0,
  .moverect     = 0,
  .erase        = &state_erase,
  .initpen      = 0,
  .setpenattr   = 0,
  .settermprop  = 0,
  .setmousefunc = 0,
  .bell         = 0,
  .resize       = 0
};


int csi_handler(const char *leader, const long args[], int argcount, const char *intermed, char command, void *user) {
  if(command == 'J') {
    printf("************************* this clear\n");
    if(!regis_recent()) regis_clear();
    inline_data_clear();
    redraw_required();
  }

  return 0;
}

int dcs_handler(const char *command,size_t cmdlen,void *user) {

  if(cmdlen < 3) return 0;

  regis_processor(command+2,cmdlen);
  printf("\n");
}

int osc_handler(const char *command,size_t cmdlen,void *user) {
}

int text_handler(const char *bytes, size_t len, void *user) {
  inline_data_receive(bytes,len);
}

int esc_handler(const char *bytes, size_t len, void *user) {
}

VTermParserCallbacks cb_parser = {
  .text    = text_handler,
  .control = 0,
  .escape  = esc_handler,
  .csi     = csi_handler,
  .osc     = osc_handler,
  .dcs     = dcs_handler,
  .resize  = 0  //&parser_resize,
//  int (*text)(const char *bytes, size_t len, void *user);
//  int (*control)(unsigned char control, void *user);
//  int (*escape)(const char *bytes, size_t len, void *user);
//  int (*csi)(const char *leader, const long args[], int argcount, const char *intermed, char command, void *user);
//  int (*osc)(const char *command, size_t cmdlen, void *user);
//  int (*dcs)(const char *command, size_t cmdlen, void *user);
//  int (*resize)(int rows, int cols, void *user);
};


void terminal_resize() {

  printf("terminal resize, size: %d %d\n",display_width,display_height);

  rows = display_height/16;
  cols = display_width/8;
    
  printf("resized: %d %d\n",cols,rows);

  if(c_resize != NULL) (*c_resize)(cols,rows);

  if(vt != 0) vterm_set_size(vt,rows,cols);
}

void cursor_position(int *cursorx,int *cursory) {
  VTermPos cursorpos;
  vterm_state_get_cursorpos(vs,&cursorpos);

  *cursorx = cursorpos.col;
  *cursory = cursorpos.row;
}

void redraw_text() {
  for(int row = 0; row < rows; row++) {

    int trow = row-scroll_offset;
    bool dont_free=false;

    int glen=0;
    VTermScreenCell *rowdata=grab_row(trow,&dont_free,&glen);

    if(rowdata != 0) draw_row(rowdata,trow,row*(font_height+font_space),glen);
    
    int cursorx=0;
    int cursory=0;
    cursor_position(&cursorx,&cursory);
    if(cursory == trow) {
      int width=font_width+font_space;
      if((cursorx < cols) && (cursory < rows) && (rowdata != 0)) {
        if(rowdata[cursorx].width == 2) width+=(font_width+font_space);

         SDL_SetRenderDrawColor(renderer,0xEF,0xEF,0xEF,0xA0);
         SDL_Rect r;
         r.x = cursorx*(font_width+font_space);
         r.y = row*(font_height+font_space);
         r.w = width;
         r.h = font_height+font_space;
         SDL_RenderFillRect(renderer,&r);

      }
    }

    if((rowdata != 0) && (dont_free==false)){free(rowdata); rowdata=0;}
  }
}

void mouse_to_select_box(int   sx,int   sy,int so,
                         int   ex,int   ey,int eo,
                         int *stx,int *sty,int *etx,int *ety) {
    
  //  if(sx > ex) {int t=ex;ex=sx;sx=t;}
  //  if(sy > ey) {int t=ey;ey=sy;sy=t;}
    
    *stx=floor(((float)sx/(font_width +font_space)));
    *etx=ceil( ((float)ex/(font_width +font_space)));
    *sty=floor(((float)sy/(font_height+font_space)))-so;
    *ety=ceil( ((float)ey/(font_height+font_space)))-eo;
    
    if(*stx==0) {
      printf("is 0\n");
    }

    if(*stx==1) {
      printf("is 1\n");
    }
}

void get_text_region(int text_start_x,int text_start_y,int text_end_x,int text_end_y,uint16_t **itext,int *ilen) {

  if(text_start_y > text_end_y) {
    int t = text_start_y;
    text_start_y = text_end_y;
    text_end_y = t;
  }

  int len=0;
  uint16_t *text = malloc(10240);
  for(int y=text_start_y;y<=text_end_y;y++) {
    bool dont_free=false;
    
    int glen=0;
    VTermScreenCell *row_data = grab_row(y,&dont_free,&glen);
    
    if(row_data == 0) { text[0]=0; }
    else {
    
      //cliping for first and last lines.
      int start_x;
      if(y == text_start_y) {
        start_x = text_start_x;
      } else {
        start_x = 0;
      }
      int end_x;
      if(y==text_end_y) {
        end_x = text_end_x;
      } else {
        end_x = cols;
      }
      
      //find last non-whitespace, clip to here too.
      for(int n=cols;n>=0;n--) {
        if(row_data[n].chars[0] != ' ') {
          if(n < end_x) end_x = n;
          break;
        }
      }
      
      for(int x=start_x;x<end_x;x++) {
        if(text_end_x >= glen) {text_end_x=glen-1;}

        text[len] = row_data[x].chars[0];
        if(text[len]!=0 && (text[len]!=65535)) len++;
      }
    }
    if(!dont_free) free(row_data);

    if(y!=text_end_y) {
      text[len] = '\n';
      len++;
    }
  }
  text[len]=0;
  text[len+1]=0;
  text[len+2]=0;
  text[len+3]=0;
  text[len+4]=0;
  text[len+5]=0;
  text[len+6]=0;
  text[len+7]=0;

  *itext = text;
  *ilen  = len;
}

void redraw_selection() {
  if(draw_selection || (draw_fade_selection>0)) {
  
    int color=255;
    if(draw_fade_selection>0) {
      color=draw_fade_selection*5;
      draw_fade_selection--;
      redraw_required();
    } else {
      color=255;
    }

    select_end_scroll_offset = scroll_offset;

    int sselect_text_end_y   = select_text_end_y   + scroll_offset;
    int sselect_text_start_y = select_text_start_y + scroll_offset;

    int sx=select_text_start_x*(font_width+font_space);
    int sy=sselect_text_start_y*(font_height+font_space);
    int ex=select_text_end_x*(font_width+font_space);
    int ey=sselect_text_end_y*(font_height+font_space)-1;

    SDL_SetRenderDrawColor(renderer, color, color, color,color);
   
    // start line
    if(select_text_start_y!=select_text_end_y) { // only draw central block bound for non-central lines.
      SDL_RenderDrawLine(renderer,0,sy+(font_height+font_space),sx,sy+(font_height+font_space));
      SDL_RenderDrawLine(renderer,sx,sy,display_width-1,sy);
    } else {
      SDL_RenderDrawLine(renderer,sx,sy,ex,sy);
    }
    SDL_RenderDrawLine(renderer,sx,sy,sx,sy+(font_height+font_space));
    
    // end line
    if(select_text_start_y!=select_text_end_y) { // only draw central block bound for non-central lines.
      SDL_RenderDrawLine(renderer,ex,ey,display_width-1,ey);
      SDL_RenderDrawLine(renderer,ex,ey+(font_height+font_space),0,ey+(font_height+font_space));
    } else {
      SDL_RenderDrawLine(renderer,sx,ey+(font_height+font_space),ex,ey+(font_height+font_space));
    }
    SDL_RenderDrawLine(renderer,ex,ey+(font_height+font_space),ex,ey);

    // only draw block for non-single lines.
    if(select_text_start_y!=select_text_end_y) {
      // central block
      SDL_RenderDrawLine(renderer,0              ,sy+(font_height+font_space),0,ey+(font_height+font_space));
      SDL_RenderDrawLine(renderer,display_width-1,sy,display_width-1,ey);
    }
  }
}

void redraw_screen() {
  SDL_SetRenderDrawColor(renderer,0x00,0x00,0x00,0xff);
  SDL_RenderClear(renderer);
    
  any_blinking = false;

  redraw_text();
  redraw_selection();
  regis_render();
  inline_data_render();
  
  ngui_render();

  SDL_RenderPresent(renderer);
}



void do_sdl_init() {
    if(SDL_Init(SDL_INIT_VIDEO)<0) {
        printf("Initialisation failed");
        return;
    }
    
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE  , 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE , 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 32);
//    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    screen=SDL_CreateWindow(NULL, 0, 0, 0, 0,SDL_WINDOW_FULLSCREEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS);
    
    display_serverselect(screen);
    
    for(;;) {
      display_serverselect_run();

      int c = display_serverselect_get(open_arg1,open_arg2,open_arg3,open_arg4,open_arg5,open_arg6);
      
      // key transfer
      if(c == 1) {
        int open_ret = ssh_open_preshell(open_arg1,open_arg2,open_arg3,open_arg4,open_arg5,open_arg6);
        int result = ssh_getkeys(open_arg5,open_arg6);
        if(result == 0) display_serverselect_keyxfer_ok();
        if(result == 1) display_serverselect_keyxfer_fail();
        display_serverselect_complete();
        c_close();
        
        break;
      }

      // normal connection
      if(c == 2) {
        display_serverselect_complete();
        break;
      }
      
      // should not happen.
      if(c == -2) {
        printf("-2\n");
        break;
      }
    }
    
    SDL_GetWindowSize(screen,&display_width,&display_height);
    display_width_abs  = display_width;
    display_height_abs = display_height;

    if (screen == 0) {
        printf("Could not initialize Window");
    }
    
    renderer = SDL_CreateRenderer(screen, -1, SDL_RENDERER_ACCELERATED);
    
    //SDL_BITSPERPIXEL(format);
    
    SDL_SetRenderDrawBlendMode(renderer,
                               SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer,0x00,0x00,0x00,0xff);
    SDL_RenderClear(renderer);
    set_system_bg(255);//alpha always 255
}

void sdl_read_thread();

void console_read_init() {
  int open_ret = c_open(open_arg1,open_arg2,open_arg3,open_arg4,open_arg5,open_arg6);

  if(open_ret == -5) {
    display_serverselect_keyfailure();
  }

  if(connection_type == CONNECTION_SSH) {
    if(open_ret == 0) {
      char *fingerprintstr = ssh_fingerprintstr();
      if(open_arg4[0]==0) {
        display_serverselect_firstkey(fingerprintstr);
      }
      write_connection(open_arg1,open_arg2,open_arg3,fingerprintstr);
    }
  }
  
  terminal_resize();
}

void console_poll() {
  // sending bytes from pts to vterm
  int len;
  char buffer[10241];
  len = c_read(buffer, sizeof(buffer)-1);

  if(len > 0) {
    printf("processing buf: %s\n",buffer);
    printf("numeric       :");
    for(int n=0; n<len;n++) {
      printf("%03u ",buffer[n]);
    }
    printf("\n");

    if((buffer != 0) && (len != 0)) {
      vterm_push_bytes(vt, buffer, len);
    }
    redraw_required();
  }
  
  if(len < 0) {
    hterm_quit = true;
    return;
  }
}

void disable_virtual_kb() {
  ngui_move_button("Iesc"  ,-1000,-1000);
  ngui_move_button("Ialt"  ,-1000,-1000);
  ngui_move_button("Ictrl" ,-1000,-1000);
  ngui_move_button("Itab"  ,-1000,-1000);
      
  ngui_move_button("Iup"   ,-1000,-1000);
  ngui_move_button("Idown" ,-1000,-1000);
  ngui_move_button("Ileft" ,-1000,-1000);
  ngui_move_button("Iright",-1000,-1000);
      
  ngui_move_button("Ipaste",-1000,-1000);
 
}

void reposition_buttons() {
  int dwidth  = display_width -(display_width %16);
  int dheight = display_height-(display_height%16);
  ngui_move_button("Iesc"  ,dwidth-(16*6*3),dheight-(16*6*3));
  ngui_move_button("Ialt"  ,dwidth-(16*6*3),dheight-(16*6*1));
  ngui_move_button("Ictrl" ,dwidth-(16*6*1),dheight-(16*6*3));
  ngui_move_button("Itab"  ,dwidth-(16*6*1),dheight-(16*6*1));
      
  ngui_move_button("Iup"   ,dwidth-(16*6*2),dheight-(16*6*3));
  ngui_move_button("Idown" ,dwidth-(16*6*2),dheight-(16*6*1));
  ngui_move_button("Ileft" ,dwidth-(16*6*3),dheight-(16*6*2));
  ngui_move_button("Iright",dwidth-(16*6*1),dheight-(16*6*2));
      
  ngui_move_button("Ipaste",dwidth-(16*6*2),dheight-(16*6*2));
      
  ngui_move_button("Iclose" ,display_width-(16*6*1)     ,0);
  ngui_move_button("Ikbshow",display_width-(16*6)-(16*7),0);
  
  
  // check if close overlaps with escape
  if((display_height-(16*6*3)) > 80) {
    ngui_move_button("Iclose",dwidth-(16*6*1)     ,0);
  } else {
    ngui_move_button("Iclose",dwidth-(16*6*4),dheight-(16*6*3));
  }
  ngui_move_button("Ikbshow",display_width_abs-(16*6)-(16*7),display_height_abs-(5*16));
}

bool redraw_req=true;
int forced_recreaterenderer=0;
int last_kb_shown=-2;



void sdl_render_thread() {
  
  SDL_Event event;
  SDL_StartTextInput();
  
  for(;;) {
    if(redraw_req) {
      redraw_req=false; // should go first as draw can itself trigger a redraw.
      redraw_screen();
    } else {
      SDL_Delay(100);
    }

    SDL_Event event;
    
    int ret = 1;
    for(;ret==1;) {
    
      ret = SDL_PollEvent(&event);
      if(ret != 0) {
        sdl_read_thread(&event);
      }
    }
      
    console_poll();
    if(hterm_quit == true) return;

    if((SDL_IsScreenKeyboardShown(screen) != last_kb_shown) &&
       (last_kb_shown != -3)) {
      SDL_GetWindowSize(screen,&display_width,&display_height);
      display_width_abs = display_width;
      display_height_abs = display_height;
      
      if(SDL_IsScreenKeyboardShown(screen)) {
        display_width  = display_width_last_kb;
        display_height = display_height_last_kb;
        reposition_buttons();
      } else {
        reposition_buttons();
        disable_virtual_kb();
      }

      redraw_required();
    }
    last_kb_shown = SDL_IsScreenKeyboardShown(screen);
  }
}

void redraw_required() {
  redraw_req=true;
}


uint8_t *paste_text() {

  return iphone_paste();

/*
  uint8_t *paste_data = malloc(sizeof(uint8_t)*10240);

  FILE *r1 = popen("xclip -o","r");
  if(r1!=NULL) {

    for(size_t n=0;feof(r1) == false;n++) {
      int c = fgetc(r1);
      if(!feof(r1)) {
        paste_data[n] = c;
        paste_data[n+1] = 0;
      }
    }

    pclose(r1);
  }

  return paste_data;
*/
}

void copy_text(uint16_t *itext,int len) {

  // TODO: This needs to be updated to generate UTF8 text
  
  size_t pos=0;
  char text[20000];
  for(int n=0;n<len;n++) {
  
    size_t s = utf8proc_encode_char(itext[n],text+pos);
    pos+=s;
    text[pos  ]=0;
    text[pos+1]=0;
    text[pos+2]=0;
    text[pos+3]=0;
    text[pos+4]=0;
  }
  
  printf("copy text: %s\n",text);
  iphone_copy(text);

/*
  FILE *w1 = popen("xclip -selection c","w");
  if(w1!=NULL) {
    fprintf(w1,"%s",text);
    pclose(w1);
  }

  FILE *w2 = popen("xclip -i","w");
  if(w2==NULL) return;
  fprintf(w2,"%s",text);
  pclose(w2);
*/
 
  // execute these two commands on Linux/XWindows by default
  //echo "test" | xclip -selection c
  //echo "test" | xclip -i 
}

int delta_sum=0;

bool select_disable=false;

int last_text_point_x = -1;
int last_text_point_y = -1;
void process_mouse_event(SDL_Event *event) {
  
  if(event->type == SDL_FINGERMOTION) {
   
     SDL_Touch *t = SDL_GetTouch(event->tfinger.touchId);

     if(t->num_fingers != 0) {
       printf("fingers");
     }

     if(t->max_fingers == 2) {
       select_disable=true;
    //   SDL_Finger *f = SDL_GetFinger(t,event->tfinger.fingerId);
    //   if(f == 0) return;
       int delta = event->tfinger.dy;
       delta_sum += delta;
       printf("delta: %d\n",delta);
       printf("delta_sum: %d\n",delta_sum);
       if(delta_sum > 500) {
         scroll_offset++;
         redraw_required();
         printf("finger scroll up %d\n",scroll_offset);
         delta_sum-=500;
       }
       if(delta_sum < -500) {
         scroll_offset--;
         if(scroll_offset < 0) scroll_offset = 0;
         redraw_required();
         printf("finger scroll down %d\n",scroll_offset);
         delta_sum+=500;
       }
       return; // prevent select code from running.
     } else {
       select_disable=false;
       // select text
//       select_start_scroll_offset = scroll_offset;
//       select_start_x = event->button.x;
//       select_start_y = event->button.y;
//       select_end_x = event->button.x;
//       select_end_y = event->button.y;
//       draw_selection = true;
     }
  }
  
  if(event->type == SDL_FINGERUP) {
//    draw_selection = false;
  }


  if(select_disable) {
    draw_selection = false;
    return;
  }

  if((event->type != SDL_MOUSEMOTION) && (event->type != SDL_MOUSEBUTTONUP) && (event->type != SDL_MOUSEBUTTONDOWN)) return;

  int mouse_x = event->motion.x;
  int mouse_y = event->motion.y;

  if(event->button.button == SDL_BUTTON_WHEELUP) {
    printf("wheel up\n");
    scroll_offset++;
    redraw_required();
    printf("scroll offset %d\n",scroll_offset);
  } else
  if(event->button.button == SDL_BUTTON_WHEELDOWN) {
    printf("wheel down\n");
    scroll_offset--;
    if(scroll_offset < 0) scroll_offset = 0;
    redraw_required();
    printf("scroll offset %d\n",scroll_offset);
  } else
  if(event->type == SDL_MOUSEMOTION    ) {

    if(draw_selection == true) {

      printf("motion: %d %d\n",event->button.x,event->button.y);
      //if(event->button.y <= 0            ) scroll_offset++;
      //if(event->button.y >= (screen->h-1)) {if(scroll_offset != 0) scroll_offset--;}

      select_end_scroll_offset = scroll_offset;
      mouse_to_select_box(select_start_x,select_start_y,select_start_scroll_offset,
                            select_end_x  ,select_end_y,select_end_scroll_offset,
                           &select_text_start_x, &select_text_start_y, &select_text_end_x, &select_text_end_y);

      select_end_x = event->button.x;
      select_end_y = event->button.y;
      redraw_required();
    }
  } else
  if(event->type == SDL_MOUSEBUTTONUP  ) {
    draw_selection = false;

    int xdelta = select_start_x-select_end_x;
    if(xdelta < 0) xdelta = 0-xdelta;
    int ydelta = select_start_y-select_end_y;
    if(ydelta < 0) ydelta = 0-ydelta;
    
    if((xdelta < 15) && (ydelta < 15)) {
    
      // this was a selection less than a single character in size
      // in these cases we don't actually process a selection.
      
      // we note the time, if there was another selection recently then this is a word selection
      // from this point we flood out left and right and select this text.
      
      // we also need to briefly display that this was selected.
    
      int text_start_x;
      int text_start_y;
      int text_end_x;
      int text_end_y;
      select_end_scroll_offset = scroll_offset;
      mouse_to_select_box(select_start_x,select_start_y,select_start_scroll_offset,
                            select_end_x, select_end_y ,select_end_scroll_offset,
                           &text_start_x, &text_start_y, &text_end_x, &text_end_y);
    
      if((last_text_point_x == text_start_x) &&
         (last_text_point_y == text_start_y)) {
         
         int word_start_x=-1;
         int word_end_x  =-1;
         
         // grab text for this line
         uint16_t *text=0;
         int len=0;
         get_text_region(0,text_start_y,cols,text_start_y,&text,&len);
         
         printf("text %d: ",len);
         for(int n=0;n<len;n++) {
           printf("%c",text[n]);
         }
         printf("\n");
         
         // find left bound
         for(int n=text_start_x;n>=0;n--) {
           if((text[n] == ' ') || (text[n] == '\n')) {
             word_start_x=n+1;
             break;
           }
         }
         
         // find right bound
         for(int n=text_start_x;(n<cols) && (n<len);n++) {
           if((text[n] == ' ') || (text[n] == '\n')) {
             word_end_x=n-1;
             break;
           }
         }
         if(word_end_x==-1) {word_end_x=len-1;}
         
         // copy single word
         printf("copy: %d %d %d %d\n",word_start_x,text_start_y,word_end_x,text_start_y);

         if(len != 0) copy_text(text+word_start_x,word_end_x-word_start_x+1);
         if(text != 0) free(text);
         
         select_text_start_x=word_start_x;
         select_text_end_x  =word_end_x+1;
         select_text_start_y=text_start_y;
         select_text_end_y  =text_start_y;
         draw_fade_selection=50;         
      } else {
        draw_selection=false;
        draw_fade_selection=0;
      }
      
      redraw_required();
      last_text_point_x = text_start_x;
      last_text_point_y = text_start_y;
      return;
    }
  
    int text_start_x;
    int text_start_y;
    int text_end_x;
    int text_end_y;
    select_end_scroll_offset = scroll_offset;
    mouse_to_select_box(select_start_x,select_start_y,select_start_scroll_offset,
                          select_end_x  ,select_end_y,select_end_scroll_offset,
                         &text_start_x, &text_start_y, &text_end_x, &text_end_y);

    select_text_start_x=text_start_x;
    select_text_start_y=text_start_y;
    select_text_end_x=text_end_x;
    select_text_end_y=text_end_y;

    uint16_t *text=0;
    int      len=0;

    if(text_end_x<text_start_x) {int c=text_end_x; text_end_x=text_start_x;text_start_x=c;}
    if(text_end_y<text_start_y) {int c=text_end_y; text_end_y=text_start_y;text_start_y=c;}

    printf("copy: %d %d %d %d\n",text_start_x,text_start_y,text_end_x,text_end_y);
    get_text_region(text_start_x,text_start_y,text_end_x,text_end_y,&text,&len);

    if(len != 0) copy_text(text,len);
    if(text != 0) free(text);
    redraw_required();
    
    draw_fade_selection=50;
      
  } else
  if(event->type == SDL_MOUSEBUTTONDOWN) {
    select_start_scroll_offset = scroll_offset;
    select_start_x = event->button.x;
    select_start_y = event->button.y;
    select_end_x = event->button.x;
    select_end_y = event->button.y;
    select_text_start_x=-1;
    select_text_end_x=-1;
    select_text_start_y=-1;
    select_text_end_y=-1;
    
    draw_selection = true;
    draw_fade_selection=0;
  }
}

void sdl_read_thread(SDL_Event *event) {
  ngui_receive_event(event);

  process_mouse_event(event);
    
/*  if(event->type == SDL_QUIT) {
    hterm_quit = true;
    return;
  }
*/
    if(forced_recreaterenderer>1) forced_recreaterenderer--;
    
    if((forced_recreaterenderer==1) ||
       ((event->type == SDL_WINDOWEVENT) &&
       ((event->window.event == SDL_WINDOWEVENT_RESIZED) || (event->window.event == SDL_WINDOWEVENT_RESTORED)))
      ) {
        forced_recreaterenderer=0;
        SDL_GetWindowSize(screen,&display_width_abs,&display_height_abs);

        display_width  = event->window.data1;
        display_height = event->window.data2;

        if(SDL_IsScreenKeyboardShown(screen)) {
          display_width  = display_width_last_kb;
          display_height = display_height_last_kb;
        }
        regis_resize(display_width_abs,display_height_abs);
        inline_data_resize(display_width_abs,display_height_abs);

        SDL_DestroyRenderer(renderer);
        renderer = SDL_CreateRenderer(screen, -1, SDL_RENDERER_ACCELERATED);
        ngui_set_renderer(renderer, redraw_required);
        nunifont_initcache();
        SDL_StartTextInput();

        terminal_resize();
        SDL_RaiseWindow(screen);
        if(SDL_IsScreenKeyboardShown(screen)) {
          reposition_buttons();
        } else {
          reposition_buttons();
          disable_virtual_kb();
        }
        redraw_required();
    }
    
    if((event->type == SDL_WINDOWEVENT) && (event->window.event == SDL_WINDOWEVENT_MOVED)) {
      int w = event->window.data1;
      int h = event->window.data2;

      display_width  = w;
      display_height = h;

      if(SDL_IsScreenKeyboardShown(screen)) {
        display_width_last_kb  = w;
        display_height_last_kb = h;
      }
      terminal_resize();
      
      if(SDL_IsScreenKeyboardShown(screen)) {
        reposition_buttons();
      } else {
        reposition_buttons();
        disable_virtual_kb();
      }
    }
    
    printf("event\n");
    if(event->type == SDL_TEXTINPUT) {
        printf("herm text input: %s\n",event->text.text);
        char buffer[255];
        
        strcpy(buffer, event->text.text);
        
        if(buffer[0] == 10) buffer[0]=13; // hack round return sending 10, which causes issues for e.g. nano.
                                          // really this should be a full utf8 decode/reencode.
        
        if(hterm_next_key_ctrl == true) {
          int i=buffer[0];
          if(i>=97) i = i-97+65;
          i-=64;
          buffer[0]=i;
          hterm_next_key_ctrl = false;
        }
        c_write(buffer,strlen(buffer));
    }
    
    if(event->type == SDL_TEXTEDITING) {
        printf("hterm text editing\n");
    }

    if(event->type == SDL_KEYDOWN) {
        printf("hterm key down\n");
 
 //int index = keyToIndex(event.key.keysym);
   SDL_Scancode scancode = event->key.keysym.scancode;
/*        if(scancode == SDL_SCANCODE_RETURN) {
            char buf[4];
            buf[0] = 13;
            buf[1] = 0;

            c_write(buf,1);
        }
*/        
        if(scancode == SDL_SCANCODE_DELETE) {
            char buf[4];
            buf[0] = 127;
            buf[1] = 0;
            
            c_write(buf,1);
        }
 
//    }
        
      scroll_offset = 0;
//      if(event.key.keysym.sym == SDLK_LSHIFT) continue;
//      if(event.key.keysym.sym == SDLK_RSHIFT) continue;
      if(scancode == SDL_SCANCODE_LEFT) {
        char buf[4];
        buf[0] = 0x1b;
        buf[1] = 'O';
        buf[2] = 'D';
        buf[3] = 0;
        c_write(buf,3);
      } else 
      if(scancode == SDL_SCANCODE_RIGHT) {
        char buf[4];
        buf[0] = 0x1b;
        buf[1] = 'O';
        buf[2] = 'C';
        buf[3] = 0;
        c_write(buf,3);
      } else 
      if(scancode == SDL_SCANCODE_UP) {
        char buf[4];
        buf[0] = 0x1b;
        buf[1] = 'O';
        buf[2] = 'A';
        buf[3] = 0;
        c_write(buf,3);
      } else 
      if(scancode == SDL_SCANCODE_DOWN) {
        char buf[4];
        buf[0] = 0x1b;
        buf[1] = 'O';
        buf[2] = 'B';
        buf[3] = 0;
        c_write(buf,3);
      } else {
  //    if((event.key.keysym.sym == SDLK_p) && (keystate[SDLK_LCTRL])) {

        // perform text paste
  //      uint8_t *text = paste_text();
  //      if(text != 0) {
  //        c_write(text,strlen(text));
  ////        free(text);
  //      }
 //     } else {
 
        // normal character
/*        char buf[2];
        buf[0] = event->key.keysym.sym;
        buf[1]=0;
        if(buf[0] != 0) {
        c_write(buf,1);
        }*/
   // //  }
    }
    }

/*
    if(event->type == SDL_VIDEORESIZE) {
      printf("resize detected A\n");
      new_screen_size_x = event->resize.w;
      new_screen_size_y = event->resize.h;
      new_screen_size   = true;

      redraw_required();
    }
  }
 */
}

void timed_repeat() {

  for(;;) {
    SDL_Delay(100);
    if(draw_selection == true) {

      if(select_end_y <= 0            ) scroll_offset++;
////      if(select_end_y >= (screen->h-1)) {if(scroll_offset != 0) scroll_offset--;}

      redraw_required();
    }

    nunifont_blinktimer();
    if(any_blinking) redraw_required();
  }

}

void vterm_initialisation() {
  vt=0;

  rows = display_height/16;
  cols = display_width/8;

  printf("init rows: %d cols: %d\n",rows,cols);
  vt = vterm_new(rows, cols);

  vts = vterm_obtain_screen(vt);
  vs  = vterm_obtain_state(vt);
  vterm_state_set_bold_highbright(vs,1);

  vterm_screen_enable_altscreen(vts,1);

  vterm_screen_set_callbacks(vts, &cb_screen, NULL);

  vterm_state_set_backup_callbacks(vs,&cb_state,0);

  vterm_screen_set_damage_merge(vts, VTERM_DAMAGE_SCROLL);
  vterm_set_parser_backup_callbacks(vt , &cb_parser, NULL);

  vterm_screen_reset(vts, 1);
  vterm_parser_set_utf8(vt,1); // should be vts?
}


bool ssh_received = false;
char ssh_hostname[100];
char ssh_username[100];
char ssh_password[100];

int prompt_id = 0;

void receive_ssh_info(char *o1,char *o2,char *o3) {

  strcpy(ssh_hostname,o1);
  strcpy(ssh_username,o2);
  strcpy(ssh_password,o3);

  ssh_received=true;

//  ngui_delete_info_prompt(prompt_id);
}

void virtual_kb_up(char *c) {
  printf("VIRTUAL UP\n");
  SDL_SendKeyboardKey(SDL_PRESSED,SDL_SCANCODE_UP);
  SDL_SendKeyboardKey(SDL_RELEASED,SDL_SCANCODE_UP);
}

void virtual_kb_down(char *c) {
  printf("VIRTUAL DOWN\n");
  SDL_SendKeyboardKey(SDL_PRESSED,SDL_SCANCODE_DOWN);
  SDL_SendKeyboardKey(SDL_RELEASED,SDL_SCANCODE_DOWN);
}

void virtual_kb_left(char *c) {
  printf("VIRTUAL DOWN\n");
  SDL_SendKeyboardKey(SDL_PRESSED,SDL_SCANCODE_LEFT);
  SDL_SendKeyboardKey(SDL_RELEASED,SDL_SCANCODE_LEFT);
}

void virtual_kb_right(char *c) {
  printf("VIRTUAL DOWN\n");
  SDL_SendKeyboardKey(SDL_PRESSED,SDL_SCANCODE_RIGHT);
  SDL_SendKeyboardKey(SDL_RELEASED,SDL_SCANCODE_RIGHT);
}

void virtual_kb_esc(char *c) {
  printf("VIRTUAL ESC\n");
  char text[5];
  text[0] = 27;
  text[1] = 0;
  SDL_SendKeyboardText(text);
  SDL_SendKeyboardKey(SDL_PRESSED,SDL_SCANCODE_ESCAPE);
  SDL_SendKeyboardKey(SDL_RELEASED,SDL_SCANCODE_ESCAPE);
}

void virtual_kb_ctrl(char *c) {
  printf("VIRTUAL CTRL\n");
  hterm_next_key_ctrl=true;
//  SDL_SendKeyboardKey(SDL_PRESSED,SDL_SCANCODE_CTRL);
}

void virtual_kb_alt(char *c) {
  printf("VIRTUAL ALT\n");
  SDL_SendKeyboardKey(SDL_PRESSED,SDL_SCANCODE_RALT);
  SDL_SendKeyboardKey(SDL_RELEASED,SDL_SCANCODE_RALT);
}

void virtual_kb_tab(char *c) {
  printf("VIRTUAL TAB\n");
  char text[5];
  text[0] = '\t';
  text[1] = 0;
  SDL_SendKeyboardText(text);
  SDL_SendKeyboardKey(SDL_PRESSED,SDL_SCANCODE_TAB);
  SDL_SendKeyboardKey(SDL_RELEASED,SDL_SCANCODE_TAB);
}

void virtual_kb_paste(char *c) {
  printf("VIRTUAL PASTE\n");
  // perform text paste
  uint8_t *text = paste_text();
  if(text != 0) {
    c_write(text,strlen(text));
//    free(text);
  }
}

void virtual_kb_kbshow(char *c) {
  printf("VIRTUAL KBSHOW\n");
  SDL_StartTextInput();
}

void virtual_kb_close(char *c) {
  printf("VIRTUAL CLOSE\n");
  c_close();
}


int main(int argc, char **argv) {
    
  connection_type = CONNECTION_LOCAL; // replace with commandline lookup
  if(argc > 1) {
    if(strcmp(argv[1],"ssh") == 0) {
      connection_type = CONNECTION_SSH; // replace with commandline lookup
    }
  }
  // iPhone version only supports ssh connections.
  #ifdef IPHONE_BUILD
    connection_type = CONNECTION_SSH;
  #endif
    
  if(connection_type == CONNECTION_LOCAL) {
    c_open   = &local_open;
    c_close  = &local_close;
    c_write  = &local_write;
    c_read   = &local_read;
    c_resize = &local_resize;
  } else
  if(connection_type == CONNECTION_SSH) {
    c_open   = &ssh_open;
    c_close  = &ssh_close;
    c_write  = &ssh_write;
    c_read   = &ssh_read;
    c_resize = &ssh_resize;
  }    
    
  do_sdl_init();
  regis_init(display_width_abs,display_height_abs);
  inline_data_init(display_width_abs,display_height_abs);
  
  ngui_set_renderer(renderer, redraw_required);
  
  // set intial button positions, these should get overwritten almost right away anyway.
  int dwidth  = display_width -(display_width %16);
  int dheight = display_height-(display_height%16);
  ngui_add_button(dwidth-(16*6*3),dheight-(16*6*3),"Iesc"  ,virtual_kb_esc  );
  ngui_add_button(dwidth-(16*6*3),dheight-(16*6*1),"Ialt"  ,virtual_kb_alt  );
  ngui_add_button(dwidth-(16*6*1),dheight-(16*6*3),"Ictrl" ,virtual_kb_ctrl );
  ngui_add_button(dwidth-(16*6*1),dheight-(16*6*1),"Itab"  ,virtual_kb_tab  );

  ngui_add_button(dwidth-(16*6*2),dheight-(16*6*3),"Iup"   ,virtual_kb_up   );
  ngui_add_button(dwidth-(16*6*2),dheight-(16*6*1),"Idown" ,virtual_kb_down );
  ngui_add_button(dwidth-(16*6*3),dheight-(16*6*2),"Ileft" ,virtual_kb_left );
  ngui_add_button(dwidth-(16*6*1),dheight-(16*6*2),"Iright",virtual_kb_right);

  ngui_add_button(dwidth-(16*6*2),dheight-(16*6*2),"Ipaste",virtual_kb_paste);

  // check if close overlaps with escape
  if((display_height-(16*6*3)) > 80) {
    ngui_add_button(dwidth-(16*6*1),               0,"Iclose" ,virtual_kb_close);
  } else {
    ngui_add_button(dwidth-(16*6*4),dheight-(16*6*3),"Iclose" ,virtual_kb_close);
  }
  ngui_add_button(display_width_abs-(16*6)-(16*7),display_height_abs-(5*16),"Ikbshow",virtual_kb_kbshow);

    
  nunifont_load_staticmap(__fontmap_static,__widthmap_static,__fontmap_static_len,__widthmap_static_len);

  SDL_GetWindowSize(screen,&display_width,&display_height);
  display_width_abs = display_width;
  display_height_abs = display_height;
  
  vterm_initialisation();



  begin_background_task();
  for(;;) {
    console_read_init();
    sdl_render_thread();
    
    c_close();
  
    printf("ihere0\n");
    display_server_select_closedlg();
    printf("ihere1\n");
  
    hterm_quit=false;
    SDL_StopTextInput();
    
    SDL_Quit(); // need this to allow me to draw server selection screen.
    do_sdl_init();
    ngui_set_renderer(renderer, redraw_required);
    nunifont_initcache();
    vterm_free(vt);
    vterm_initialisation();
  }

  return 0;
}
