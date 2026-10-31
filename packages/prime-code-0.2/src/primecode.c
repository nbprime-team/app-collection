#include <stdint.h>
#include "unifont_font.h"

#define LCD_W 320
#define LCD_H 240
#define TITLE_H 22
#define LINE_H 20
#define LINE_NUMBER_W 30
#define GUTTER 4
#define SCROLLBAR_W 10
#define MAX_LINES 512
#define MAX_LINE 512
#define MAX_COMPLETION 16
#define EVENT_WORDS 18

#define EV_KEYDOWN 0x10u
#define EV_KEYUP 0x00100000u
#define EV_TOUCH_BEGIN 0x1u
#define EV_TOUCH_MOVE 0x2u
#define EV_TOUCH_END 0x8u
#define HP_ESC 4
#define HP_SHIFT 41
#define HP_ALPHA 36
#define HP_UP 2
#define HP_DOWN 12
#define HP_LEFT 7
#define HP_RIGHT 8
#define HP_ENTER 18
#define HP_DELETE 19
#define HP_SYMB 47
#define HP_PLOT 48

extern void *prime_sys_get_lcd(void);
extern int prime_sys_get_event(void *event);
extern void prime_sys_sleep(uint32_t ms);

static uint32_t framebuf[LCD_W * LCD_H] __attribute__((aligned(32)));
static char lines[MAX_LINES][MAX_LINE];
static int line_count = 1, cursor_row, cursor_col, scroll_y, scroll_col;
static int anchor_row = -1, anchor_col;
static int dark_theme = 1, hppl_mode, shift_state, alpha_state;
static int completion_visible, completion_selected, completion_count;
static char completion_items[MAX_COMPLETION][32];
static int search_mode, replace_mode, search_len, replace_len;
static char search_text[64], replace_text[64];
static int match_rows[MAX_COMPLETION], match_cols[MAX_COMPLETION], match_count;
static int selector_mode = 1, selector_selected, quit;
static int touch_active, touch_selecting, touch_scrollbar;
static int touch_last_x, touch_last_y;
static int touch_selector_row = -1;

static const int key_ids[] = {14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,37,38,39,40,42,43,44,45,47,48,49,50};
static const char *alpha_lower[] = {"a","b","c","d","e","__del__","f","g","h","i","j","k","l","m","n","o","__enter__","p","q","r","s","t","u","v","w","x","y","z","#",":","\"","."," ",";"};
static const char *alpha_upper[] = {"A","B","C","D","E","__del__","F","G","H","I","J","K","L","M","N","O","__enter__","P","Q","R","S","T","U","V","W","X","Y","Z","#",":","\"","."," ",";"};
static const char *normal_keys[] = {"__indent__","__tool__","","x","/","__del__","^","sin()","cos()","tan()","ln()","log()","^2","-","()",",","__enter__","*10^","7","8","9","/","4","5","6","*","1","2","3","-","0","."," ","+"};
static const char *python_words[] = {"False","None","True","and","as","assert","async","await","break","class","continue","def","del","elif","else","except","for","from","global","if","import","in","is","lambda","not","or","pass","raise","return","try","while","with","yield","abs","bool","dict","enumerate","eval","float","input","int","len","list","map","max","min","print","range","str","sum","tuple","type"};
static const char *hppl_words[] = {"BEGIN","END","LOCAL","EXPORT","IF","THEN","ELSE","ELSEIF","FOR","FROM","TO","STEP","DOWNTO","DO","WHILE","REPEAT","UNTIL","BREAK","CONTINUE","RETURN","PRINT","INPUT","MSGBOX","WAIT","CHOOSE","CASE","DEFAULT","ENDCASE","AND","OR","NOT","XOR","PI","MIN","MAX","FLOOR","CEILING","ROUND","MOD","DIM","SIZE","LIST","MATRIX","FUNCTION","PROGRAM","VIEW","START","ENDVIEW"};

static uint32_t *lcd_framebuffer(void) { uint32_t *lcd = (uint32_t *)prime_sys_get_lcd(); uint32_t **table; if (!lcd) return 0; table = *(uint32_t ***)lcd; return table ? *(uint32_t **)((uint8_t *)table + 0x10) : 0; }
static void fill(uint32_t *fb,int x,int y,int w,int h,uint32_t c) { int xx,yy; for(yy=y;yy<y+h;++yy) for(xx=x;xx<x+w;++xx) if((unsigned)xx<LCD_W&&(unsigned)yy<LCD_H) fb[yy*LCD_W+xx]=c; }
static void pixel(uint32_t *fb,int x,int y,uint32_t c) { if((unsigned)x<LCD_W&&(unsigned)y<LCD_H)fb[y*LCD_W+x]=c; }
static void glyph(uint32_t *fb,int x,int y,char ch,uint32_t c) { uint8_t w; const uint16_t *g=unifont_glyph((unsigned char)ch,&w); int r,k; for(r=0;r<16;++r)for(k=0;k<w;++k)if(g[r]&(uint16_t)(1u<<(w-1-k)))pixel(fb,x+k,y+r,c); }
static void text(uint32_t *fb,int x,int y,const char *s,uint32_t c) { while(*s){if(*s!=' ')glyph(fb,x,y,*s,c);x+=10;++s;} }
static int ident(char c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'; }
static int same(const char *a,const char *b) { while(*a&&*a==*b)++a,++b; return *a==0&&*b==0; }
static int known(const char *word) { const char **pool=hppl_mode?hppl_words:python_words; int n=hppl_mode?(int)(sizeof(hppl_words)/sizeof(*hppl_words)):(int)(sizeof(python_words)/sizeof(*python_words)); int i; for(i=0;i<n;++i)if(same(word,pool[i]))return 1; return 0; }
static uint32_t syntax_color(const char *s,int pos) { char word[40]; int a=pos,n=0; if(!hppl_mode&&s[pos]=='#')return 0xff55aa55u; if(hppl_mode&&s[pos]=='/'&&s[pos+1]=='/')return 0xff55aa55u; if(s[pos]=='\''||s[pos]=='\"')return 0xffffaa00u; if(s[pos]>='0'&&s[pos]<='9')return 0xffff8800u; while(a&&ident(s[a-1]))--a; while(ident(s[a])&&n<39)word[n++]=s[a++]; word[n]=0; return known(word)?0xff55aaffu:(dark_theme?0xffffffffu:0xff202020u); }
static void clear_selection(void) { anchor_row=-1; }
static void delete_selection(void) { int ar=anchor_row,ac=anchor_col,r=cursor_row,c=cursor_col,i,j; if(ar<0)return; if(ar>r||(ar==r&&ac>c)){i=ar;ar=r;r=i;i=ac;ac=c;c=i;} if(ar==r){for(i=ac;lines[r][i+c-ac];++i)lines[r][i]=lines[r][i+c-ac];lines[r][i]=0;}else{j=0;while(lines[r][c+j])++j;for(i=0;i<=j;++i)lines[ar][ac+i]=lines[r][c+i];for(i=ar+1;i<=r;++i){for(j=i;j<line_count-1;++j){int k=0;while((lines[j][k]=lines[j+1][k]))++k;}}line_count-=r-ar;}cursor_row=ar;cursor_col=ac;clear_selection();}
static void insert_text(const char *s) { int n=0,i,cap=MAX_LINE-1-cursor_col; if(anchor_row>=0)delete_selection(); if(cap<=0)return; while(s[n]&&n<cap)++n; for(i=MAX_LINE-1;i>=cursor_col+n;--i)lines[cursor_row][i]=lines[cursor_row][i-n];for(i=0;i<n;++i)lines[cursor_row][cursor_col+i]=s[i];cursor_col+=n; }
static void scan_matches(void) { int r,c,i;match_count=0;if(!search_len)return;for(r=0;r<line_count&&match_count<MAX_COMPLETION;++r)for(c=0;lines[r][c];++c){for(i=0;i<search_len&&lines[r][c+i]==search_text[i];++i){}if(i==search_len){match_rows[match_count]=r;match_cols[match_count++]=c;}} }
static void trigger_completion(void) { char prefix[32];int start=cursor_col,n=0,i;const char **pool=hppl_mode?hppl_words:python_words;int count=hppl_mode?(int)(sizeof(hppl_words)/sizeof(*hppl_words)):(int)(sizeof(python_words)/sizeof(*python_words));while(start&&ident(lines[cursor_row][start-1]))--start;while(start+n<cursor_col&&n<31)prefix[n]=lines[cursor_row][start+n],++n;prefix[n]=0;completion_count=0;for(i=0;i<count;++i){int j=0;while(pool[i][j]&&prefix[j]&&pool[i][j]==prefix[j])++j;if(!prefix[j]&&completion_count<MAX_COMPLETION){int k;for(k=0;pool[i][k]&&k<31;++k)completion_items[completion_count][k]=pool[i][k];completion_items[completion_count][k]=0;++completion_count;}}completion_visible=completion_count>0;completion_selected=0;}
static const char *key_text(int k) {
	int i;
	for (i = 0; i < (int)(sizeof(key_ids) / sizeof(*key_ids)); ++i)
		if (key_ids[i] == k) break;
	if (i == (int)(sizeof(key_ids) / sizeof(*key_ids))) return 0;
	if (alpha_state) return (shift_state || alpha_state == 2) ? alpha_upper[i] : alpha_lower[i];
	return normal_keys[i];
}

static void insert_key(int k) {
	const char *s = key_text(k);
	if (s && s[0] != '_') insert_text(s);
	shift_state = 0;
	if (alpha_state == 1) alpha_state = 0;
}

static void append_search_key(int k) {
	const char *s = key_text(k);
	int *len = replace_mode ? &replace_len : &search_len;
	char *dst = replace_mode ? replace_text : search_text;
	int limit = replace_mode ? (int)sizeof(replace_text) - 1 : (int)sizeof(search_text) - 1;
	int n = 0;
	if (!s || s[0] == '_') return;
	while (s[n] && *len + n < limit) {
		dst[*len + n] = s[n];
		++n;
	}
	*len += n;
	dst[*len] = 0;
	shift_state = 0;
	if (alpha_state == 1) alpha_state = 0;
}
static void handle_key(int k) { int i,j; if(selector_mode){if(k==HP_UP&&selector_selected) --selector_selected;else if(k==HP_DOWN&&selector_selected<1)++selector_selected;else if(k==HP_ENTER)selector_mode=0;else if(k==HP_ESC||k==0x83)quit=1;return;} if(search_mode){if(k==HP_ESC){search_mode=replace_mode=0;return;}if(k==HP_DELETE){if(replace_mode&&replace_len)replace_text[--replace_len]=0;else if(search_len)search_text[--search_len]=0;scan_matches();return;}if(k==HP_ENTER){if(replace_mode&&match_count){i=match_rows[0];j=match_cols[0];cursor_row=i;cursor_col=j;insert_text(replace_text);}else if(match_count){cursor_row=match_rows[0];cursor_col=match_cols[0];}return;}append_search_key(k);scan_matches();return;}if(k==HP_ESC){if(completion_visible)completion_visible=0;else if(anchor_row>=0)clear_selection();else selector_mode=1;return;}if(k==HP_SHIFT){shift_state=!shift_state;return;}if(k==HP_ALPHA){alpha_state=(alpha_state+1)%3;return;}if(k==HP_SYMB){search_mode=1;replace_mode=0;search_len=0;search_text[0]=0;return;}if(k==HP_PLOT){search_mode=replace_mode=1;search_len=replace_len=0;search_text[0]=replace_text[0]=0;return;}if(k==49){hppl_mode=!hppl_mode;completion_visible=0;return;}if(completion_visible&&(k==HP_UP||k==HP_DOWN)){if(k==HP_UP&&completion_selected) --completion_selected;if(k==HP_DOWN&&completion_selected+1<completion_count)++completion_selected;return;}if(k==HP_ENTER){if(completion_visible){insert_text(completion_items[completion_selected]);completion_visible=0;return;}if(line_count<MAX_LINES-1){for(i=line_count;i>cursor_row+1;--i)for(j=0;(lines[i][j]=lines[i-1][j]);++j){}for(i=cursor_col;lines[cursor_row][i];++i)lines[cursor_row+1][i-cursor_col]=lines[cursor_row][i];lines[cursor_row][cursor_col]=0;++line_count;++cursor_row;cursor_col=0;}return;}if(k==HP_DELETE){if(anchor_row>=0){delete_selection();return;}if(cursor_col){for(i=cursor_col;i<MAX_LINE;++i){lines[cursor_row][i-1]=lines[cursor_row][i];if(!lines[cursor_row][i-1])break;}--cursor_col;}else if(cursor_row){int n=0;while(lines[cursor_row-1][n])++n;for(i=0;i<MAX_LINE-n;++i){lines[cursor_row-1][n+i]=lines[cursor_row][i];if(!lines[cursor_row-1][n+i])break;}for(i=cursor_row;i<line_count-1;++i)for(j=0;j<MAX_LINE;++j){lines[i][j]=lines[i+1][j];if(!lines[i][j])break;}--line_count;--cursor_row;cursor_col=n;}return;}if(k==HP_UP){if(cursor_row)--cursor_row;return;}if(k==HP_DOWN){if(cursor_row+1<line_count)++cursor_row;return;}if(k==HP_LEFT){if(cursor_col)--cursor_col;else if(cursor_row){--cursor_row;cursor_col=0;while(lines[cursor_row][cursor_col])++cursor_col;}completion_visible=0;return;}if(k==HP_RIGHT){if(lines[cursor_row][cursor_col])++cursor_col;else if(cursor_row+1<line_count){++cursor_row;cursor_col=0;}completion_visible=0;return;}insert_key(k);if(k>=14&&k<=50)trigger_completion(); }
static int clamp_int(int value, int low, int high) {
	if (value < low) return low;
	if (value > high) return high;
	return value;
}

static void touch_cursor(int x, int y, int select) {
	int row, col, max_col;
	if (y < TITLE_H || x < LINE_NUMBER_W + GUTTER ||
		x >= LCD_W - SCROLLBAR_W) return;
	row = scroll_y / LINE_H + (y - TITLE_H) / LINE_H;
	row = clamp_int(row, 0, line_count - 1);
	col = scroll_col + (x - LINE_NUMBER_W - GUTTER) / 10;
	max_col = 0;
	while (lines[row][max_col]) ++max_col;
	col = clamp_int(col, 0, max_col);
	if (select && anchor_row < 0) {
		anchor_row = cursor_row;
		anchor_col = cursor_col;
	}
	cursor_row = row;
	cursor_col = col;
}

static void touch_scroll(int y) {
	int content_h = LCD_H - TITLE_H;
	int visible = content_h / LINE_H;
	int max_scroll = line_count > visible ? (line_count - visible) * LINE_H : 0;
	int thumb = line_count > visible ? (visible * content_h) / line_count : content_h;
	int track = content_h - thumb;
	int offset = y - TITLE_H - thumb / 2;
	if (track <= 0 || max_scroll <= 0) {
		scroll_y = 0;
		return;
	}
	offset = clamp_int(offset, 0, track);
	scroll_y = (offset * max_scroll) / track;
}

static void handle_touch(uint32_t action, int x, int y) {
	if (action == EV_TOUCH_BEGIN) {
		touch_active = 1;
		touch_selecting = 0;
		touch_scrollbar = x >= LCD_W - SCROLLBAR_W;
		touch_last_x = x;
		touch_last_y = y;
		if (selector_mode) {
			touch_selector_row = -1;
			if (y >= 32 && y < 64) touch_selector_row = 0;
			else if (y >= 64 && y < 96) touch_selector_row = 1;
			if (touch_selector_row >= 0) selector_selected = touch_selector_row;
		} else if (touch_scrollbar) {
			touch_scroll(y);
		} else {
			touch_cursor(x, y, 0);
		}
	} else if (action == EV_TOUCH_MOVE && touch_active) {
		if (selector_mode) return;
		if (touch_scrollbar) touch_scroll(y);
		else {
			touch_selecting = 1;
			touch_cursor(x, y, 1);
		}
		touch_last_x = x;
		touch_last_y = y;
	} else if (action == EV_TOUCH_END && touch_active) {
		if (selector_mode && touch_selector_row >= 0 &&
			y >= 32 + touch_selector_row * 32 &&
			y < 64 + touch_selector_row * 32) {
			selector_mode = 0;
		} else if (!selector_mode && !touch_scrollbar && touch_selecting &&
			anchor_row == cursor_row && anchor_col == cursor_col) {
			clear_selection();
		}
		touch_active = 0;
		touch_selecting = 0;
		touch_scrollbar = 0;
		touch_selector_row = -1;
	}
}

static void handle_event(uint32_t *e) {
	uint32_t type = e[1];
	uint32_t record = e[7];
	uint32_t action = record & 0xffffu;
	int x = (int)(e[8] & 0xffffu);
	int y = (int)(e[9] & 0xffffu);
	if (type == 0x00100010u || type == EV_KEYDOWN) {
		handle_key((int)action);
	} else if (type != EV_KEYUP) {
		handle_touch(action, x, y);
	}
}
static void draw_editor(uint32_t *fb) { int i,j,start=scroll_y/LINE_H,y=TITLE_H-scroll_y%LINE_H,visible=(LCD_H-TITLE_H)/LINE_H;uint32_t bg=dark_theme?0xff101820u:0xfff5f1e8u,fg=dark_theme?0xffffffffu:0xff202020u;fill(fb,0,0,LCD_W,LCD_H,bg);fill(fb,0,0,LCD_W,TITLE_H,dark_theme?0xff402020u:0xffd8cdbb);text(fb,4,2,"PrimeCode",fg);text(fb,230,2,hppl_mode?"HPPL":"PY",fg);for(i=0;i<=visible&&start+i<line_count;++i,y+=LINE_H){int r=start+i;text(fb,4,y+2,"   ",dark_theme?0xff8899aau:0xff777777u);for(j=scroll_col;lines[r][j]&&LINE_NUMBER_W+GUTTER+(j-scroll_col)*10<LCD_W-SCROLLBAR_W;++j)glyph(fb,LINE_NUMBER_W+GUTTER+(j-scroll_col)*10,y,lines[r][j],syntax_color(lines[r],j));}fill(fb,LCD_W-SCROLLBAR_W,TITLE_H,SCROLLBAR_W,LCD_H-TITLE_H,dark_theme?0xff28384au:0xffc0b8a8u);y=TITLE_H+cursor_row*LINE_H-scroll_y;if(y>=TITLE_H&&y<LCD_H)fill(fb,LINE_NUMBER_W+GUTTER+(cursor_col-scroll_col)*10,y,2,LINE_H,0xffff4444u);if(search_mode){text(fb,4,204,replace_mode?"REPLACE":"SEARCH",0xffffc857u);text(fb,72,204,replace_mode?replace_text:search_text,fg);}if(completion_visible){fill(fb,LINE_NUMBER_W+GUTTER+cursor_col*10,y+LINE_H,130,completion_count*LINE_H+4,0xff002064u);for(i=0;i<completion_count;++i)text(fb,LINE_NUMBER_W+GUTTER+cursor_col*10+4,y+LINE_H+2+i*LINE_H,completion_items[i],i==completion_selected?0xffffc857u:0xffffffffu);}}
static void draw_selector(uint32_t *fb) {fill(fb,0,0,LCD_W,LCD_H,0xff101820u);fill(fb,0,0,LCD_W,TITLE_H,0xff402020u);text(fb,4,2,"PrimeCode",0xffffffffu);text(fb,4,48,"#New program",selector_selected==0?0xffffc857u:0xffffffffu);text(fb,4,72,"PrimeCode",selector_selected==1?0xffffc857u:0xffffffffu);text(fb,4,218,"ENTER OPEN  ESC EXIT",0xffb8c7d9u);}
int main(void *config,void *reserved) { uint32_t *lcd,event[EVENT_WORDS];(void)config;(void)reserved;lcd=lcd_framebuffer();if(!lcd)return 0;while(!quit){if(prime_sys_get_event(event))handle_event(event);if(!selector_mode){if(cursor_row*LINE_H<scroll_y)scroll_y=cursor_row*LINE_H;if((cursor_row+1)*LINE_H>scroll_y+LCD_H-TITLE_H)scroll_y=(cursor_row+1)*LINE_H-(LCD_H-TITLE_H);}if(selector_mode)draw_selector(framebuf);else draw_editor(framebuf);{int i;for(i=0;i<LCD_W*LCD_H;++i)lcd[i]=framebuf[i];}prime_sys_sleep(20);}return 0;}
