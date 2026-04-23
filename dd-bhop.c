/*
 * dd-bhop — bhop + air control for Devil Daggers
 *
 * Usage:
 *   dd-bhop              — bhop only (hold space)
 *   dd-bhop --strafe     — bhop + air control boost
 *   dd-bhop --diag       — live diagnostic
 *   dd-bhop --aim        — dagger landing prediction (read-only)
 *   dd-bhop --teleport X Z
 *
 * Requires dd-bhop.conf (see example.conf)
 */

#define _GNU_SOURCE
#include <sys/uio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <math.h>
#include <time.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <gtk/gtk.h>
#include <gtk-layer-shell.h>

#define POLL_US  2000
#define LOG_PATH "/tmp/dd-strafe.log"

struct cfg {
	unsigned long off_globals, off_arena, off_hero;
	unsigned long h_pos_x, h_pos_y, h_pos_z;
	unsigned long h_vel_x, h_vel_y, h_vel_z;
	unsigned long h_yaw, h_pitch, h_alive;
	unsigned long h_spd_a, h_spd_b, h_spd_c, h_spd_d, h_spd_e;
	unsigned long h_frict, h_timing, h_perf;
	uint32_t state_gnd, state_air, state_fall;
	float timing_val;
	float boost_accel;
	float boost_spd_c, boost_spd_d, boost_spd_e;
	float boost_frict;
	float gravity_cut, max_boost_alt;
	float dagger_speed, dagger_gravity, dagger_up;
	float fov;
};

static pid_t        pid = 0;
static volatile int running = 1;
static FILE        *logfile = NULL;
static void sighandler(int s) { (void)s; running = 0; }

#define LOG(fmt, ...) do { if(logfile){fprintf(logfile,fmt,##__VA_ARGS__);fflush(logfile);} } while(0)

/* config parser */
static int parse_ulong(const char *s, unsigned long *out) {
	char *end; *out = strtoul(s, &end, 0);
	return (*end=='\0'||*end=='\n'||*end=='\r') ? 0 : -1;
}
static int parse_float(const char *s, float *out) {
	char *end; *out = strtof(s, &end);
	return (*end=='\0'||*end=='\n'||*end=='\r') ? 0 : -1;
}
static int parse_uint(const char *s, uint32_t *out) {
	char *end; *out = (uint32_t)strtoul(s, &end, 0);
	return (*end=='\0'||*end=='\n'||*end=='\r') ? 0 : -1;
}

static int load_config(const char *path, struct cfg *c) {
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	char line[256];
	int n = 0;
	while (fgets(line, sizeof(line), f)) {
		if (line[0]=='#'||line[0]=='\n') continue;
		char *nl = strchr(line,'\n'); if(nl)*nl=0;
		nl = strchr(line,'\r'); if(nl)*nl=0;
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		char *k = line, *v = eq+1;
		while (*v==' ') v++;
		{ char *e=k+strlen(k); while(e>k&&*(e-1)==' ')*--e=0; }
		#define C(kname, field, parse, T) \
			if(strcmp(k,kname)==0){T t;if(parse(v,&t)==0){c->field=t;n++;}continue;}
		C("off_globals", off_globals, parse_ulong, unsigned long)
		C("off_arena",   off_arena,   parse_ulong, unsigned long)
		C("off_hero",    off_hero,    parse_ulong, unsigned long)
		C("h_pos_x", h_pos_x, parse_ulong, unsigned long)
		C("h_pos_y", h_pos_y, parse_ulong, unsigned long)
		C("h_pos_z", h_pos_z, parse_ulong, unsigned long)
		C("h_vel_x", h_vel_x, parse_ulong, unsigned long)
		C("h_vel_y", h_vel_y, parse_ulong, unsigned long)
		C("h_vel_z", h_vel_z, parse_ulong, unsigned long)
		C("h_yaw",   h_yaw,   parse_ulong, unsigned long)
		C("h_pitch", h_pitch, parse_ulong, unsigned long)
		C("h_alive", h_alive, parse_ulong, unsigned long)
		C("h_spd_a", h_spd_a, parse_ulong, unsigned long)
		C("h_spd_b", h_spd_b, parse_ulong, unsigned long)
		C("h_spd_c", h_spd_c, parse_ulong, unsigned long)
		C("h_spd_d", h_spd_d, parse_ulong, unsigned long)
		C("h_spd_e", h_spd_e, parse_ulong, unsigned long)
		C("h_frict", h_frict, parse_ulong, unsigned long)
		C("h_timing",h_timing,parse_ulong, unsigned long)
		C("h_perf",  h_perf,  parse_ulong, unsigned long)
		C("state_gnd",  state_gnd,  parse_uint, uint32_t)
		C("state_air",  state_air,  parse_uint, uint32_t)
		C("state_fall", state_fall, parse_uint, uint32_t)
		C("timing_val",  timing_val,  parse_float, float)
		C("boost_accel", boost_accel, parse_float, float)
		C("boost_spd_c", boost_spd_c, parse_float, float)
		C("boost_spd_d", boost_spd_d, parse_float, float)
		C("boost_spd_e", boost_spd_e, parse_float, float)
		C("boost_frict", boost_frict, parse_float, float)
		C("gravity_cut",   gravity_cut,   parse_float, float)
		C("max_boost_alt", max_boost_alt, parse_float, float)
		C("dagger_speed",    dagger_speed,    parse_float, float)
		C("dagger_gravity",  dagger_gravity,  parse_float, float)
		C("dagger_up",       dagger_up,       parse_float, float)
		C("fov",             fov,             parse_float, float)
		#undef C
	}
	fclose(f);
	return n >= 10 ? 0 : -1;
}

/* memory r/w */
static int vm_rd(unsigned long a, void *b, size_t l) {
	struct iovec lv={b,l}, rv={(void*)a,l};
	return process_vm_readv(pid,&lv,1,&rv,1,0)==(ssize_t)l?0:-1;
}
static int vm_wr(unsigned long a, const void *b, size_t l) {
	struct iovec lv={(void*)b,l}, rv={(void*)a,l};
	return process_vm_writev(pid,&lv,1,&rv,1,0)==(ssize_t)l?0:-1;
}
static unsigned long rp(unsigned long a) { uint64_t v=0; vm_rd(a,&v,8); return (unsigned long)v; }
static float rf(unsigned long a) { float v=0; vm_rd(a,&v,4); return v; }
static uint32_t ru(unsigned long a) { uint32_t v=0; vm_rd(a,&v,4); return v; }
static void wf(unsigned long a, float v) { vm_wr(a,&v,4); }
static void wu(unsigned long a, uint32_t v) { vm_wr(a,&v,4); }

static unsigned long get_base(void) {
	char p[64]; snprintf(p,64,"/proc/%d/maps",pid);
	FILE *f=fopen(p,"r"); if(!f) return 0;
	char line[512]; unsigned long b=0;
	while(fgets(line,sizeof(line),f))
		if(strstr(line,"devildaggers")&&strstr(line,"r-xp")){b=strtoul(line,NULL,16);break;}
	fclose(f); return b;
}
static int resolve(const struct cfg *c, unsigned long base, unsigned long *hero) {
	unsigned long g=rp(base+c->off_globals); if(g<0x10000) return -1;
	unsigned long a=rp(g+c->off_arena);      if(a<0x10000) return -1;
	unsigned long h=rp(a+c->off_hero);       if(h<0x10000) return -1;
	*hero=h; return 0;
}

static int space_held(Display *dpy, KeyCode kc) {
	char keys[32];
	XQueryKeymap(dpy, keys);
	return (keys[kc >> 3] >> (kc & 7)) & 1;
}

static const char *state_str(const struct cfg *c, uint32_t alive) {
	if (alive == c->state_gnd)  return "GND";
	if (alive == c->state_air)  return "AIR";
	if (alive == c->state_fall) return "FALL";
	if (alive == 0)             return "DEAD";
	return "??";
}

static void run_diag(const struct cfg *c, unsigned long base) {
	unsigned long hero=0; float px=0,pz=0;
	struct timespec prev={0};
	printf("diag — Ctrl-C to stop\n\n");
	while(running){
		if(resolve(c,base,&hero)<0){usleep(10000);continue;}
		struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
		float dt=prev.tv_sec?(float)(now.tv_sec-prev.tv_sec)+(float)(now.tv_nsec-prev.tv_nsec)/1e9f:0.001f;
		float cx=rf(hero+c->h_pos_x), cy=rf(hero+c->h_pos_y), cz=rf(hero+c->h_pos_z);
		uint32_t alive=ru(hero+c->h_alive);
		float vx=rf(hero+c->h_vel_x), vy=rf(hero+c->h_vel_y), vz=rf(hero+c->h_vel_z);
		float yaw=rf(hero+c->h_yaw), sa=rf(hero+c->h_spd_a), sb=rf(hero+c->h_spd_b), fr=rf(hero+c->h_frict);
		float dx=(cx-px)/dt, dz=(cz-pz)/dt, hs=sqrtf(dx*dx+dz*dz);
		printf("\r[%s] pos=(%7.2f,%5.2f,%7.2f) vel=(%6.1f,%6.1f,%6.1f) hspd=%5.1f spd=(%.1f,%.1f) fr=%.4f  ",
		       state_str(c,alive), cx,cy,cz,vx,vy,vz,hs,sa,sb,fr);
		fflush(stdout);
		px=cx; pz=pz; prev=now; usleep(5000);
	}
	printf("\n");
}

/* recursively find a top-level X11 window whose WM_NAME contains needle */
static Window find_window_by_name(Display *dpy, Window root, const char *needle) {
	Window found = 0, dummy1, dummy2, *children;
	unsigned int nch;
	if (!XQueryTree(dpy, root, &dummy1, &dummy2, &children, &nch) || !children)
		return 0;
	for (unsigned int i = 0; i < nch && !found; i++) {
		char *name = NULL;
		if (XFetchName(dpy, children[i], &name) && name) {
			if (strcasestr(name, needle))
				found = children[i];
			XFree(name);
		}
		if (!found)
			found = find_window_by_name(dpy, children[i], needle);
	}
	XFree(children);
	return found;
}

/* get absolute x/y and w/h of a window (follows frame to parent if needed) */
static int win_geom(Display *dpy, Window w, int *x, int *y, int *ww, int *wh) {
	if (!w) return -1;
	XWindowAttributes wa;
	if (!XGetWindowAttributes(dpy, w, &wa)) return -1;
	if (wa.override_redirect || wa.class == InputOnly) {
		/* use as-is */
		*x = wa.x; *y = wa.y; *ww = wa.width; *wh = wa.height;
	} else {
		Window parent = w, root_ret, *junk;
		unsigned int nj;
		XQueryTree(dpy, parent, &root_ret, &parent, &junk, &nj);
		if (junk) XFree(junk);
		if (parent && parent != root_ret) {
			XWindowAttributes pa;
			if (XGetWindowAttributes(dpy, parent, &pa)) {
				*x = pa.x; *y = pa.y; *ww = pa.width; *wh = pa.height;
			} else {
				*x = wa.x; *y = wa.y; *ww = wa.width; *wh = wa.height;
			}
		} else {
			*x = wa.x; *y = wa.y; *ww = wa.width; *wh = wa.height;
		}
	}
	/* translate to root coords */
	Window child;
	XTranslateCoordinates(dpy, w, DefaultRootWindow(dpy), 0, 0, x, y, &child);
	return 0;
}

/* aim overlay context */
static struct {
	const struct cfg *c;
	unsigned long base;
	int ww, wh;
	float mx, my;
	int vis, tick;
	GtkWidget *win, *da;
} aim;

static gboolean aim_draw_cb(GtkWidget *w, cairo_t *cr, gpointer p) {
	(void)w; (void)p;
	if (!aim.vis) return FALSE;
	/* clear to transparent */
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);
	/* ring */
	cairo_set_source_rgba(cr, 1.0, 0.62, 0.71, 0.85);
	cairo_set_line_width(cr, 2.0);
	cairo_arc(cr, aim.mx, aim.my, 10, 0, 2 * M_PI);
	cairo_stroke(cr);
	/* center dot */
	cairo_set_source_rgba(cr, 1.0, 0.62, 0.71, 1.0);
	cairo_arc(cr, aim.mx, aim.my, 3, 0, 2 * M_PI);
	cairo_fill(cr);
	return FALSE;
}

static gboolean aim_tick_cb(gpointer p) {
	(void)p;
	if (!running) { gtk_main_quit(); return FALSE; }

	unsigned long hero = 0;
	if (resolve(aim.c, aim.base, &hero) < 0) {
		aim.vis = 0;
		gtk_widget_queue_draw(aim.da);
		return TRUE;
	}

	float px = rf(hero + aim.c->h_pos_x);
	float py = rf(hero + aim.c->h_pos_y);
	float pz = rf(hero + aim.c->h_pos_z);
	float yaw = rf(hero + aim.c->h_yaw);
	float pitch = rf(hero + aim.c->h_pitch);

	float cp = cosf(pitch), sp = sinf(pitch);
	float cy = cosf(yaw),   sy = sinf(yaw);

	float fx = sy*cp, fy = -sp, fz = -cy*cp;
	float rx = cy, ry = 0.0f, rz = sy;
	float ux = fy*rz - fz*ry;
	float uy = fz*rx - fx*rz;
	float uz = fx*ry - fy*rx;

	float dvx = aim.c->dagger_speed * fx;
	float dvy = aim.c->dagger_speed * fy + aim.c->dagger_up;
	float dvz = aim.c->dagger_speed * fz;

	float g = aim.c->dagger_gravity;
	float disc = dvy*dvy + 2.0f*g*py;
	if (disc < 0 || g <= 0.0f) { aim.vis = 0; gtk_widget_queue_draw(aim.da); return TRUE; }
	float t = (dvy + sqrtf(disc)) / g;
	if (t < 0.0f) { aim.vis = 0; gtk_widget_queue_draw(aim.da); return TRUE; }

	float lx = px + dvx*t;
	float lz = pz + dvz*t;

	float dx = lx - px, dy = -py, dz = lz - pz;
	float depth = dx*fx + dy*fy + dz*fz;
	float scr_x = dx*rx + dy*ry + dz*rz;
	float scr_y = dx*ux + dy*uy + dz*uz;

	if (depth > 0.1f) {
		float fov = aim.c->fov > 0.0f ? aim.c->fov : 90.0f;
		float tan_half = tanf(fov * (float)M_PI / 360.0f);
		float nx = scr_x / (depth * tan_half);
		float ny = scr_y / (depth * tan_half);
		aim.mx = (float)aim.ww * (nx + 1.0f) * 0.5f;
		aim.my = (float)aim.wh * (1.0f - ny) * 0.5f;
		aim.vis = 1;
	} else {
		aim.vis = 0;
	}

	if (++aim.tick % 10 == 0) {
		float ddist = sqrtf(dx*dx + dz*dz);
		uint32_t alive; vm_rd(hero + aim.c->h_alive, &alive, 4);
		printf("\r[%s] aim=(%7.2f, %7.2f) dist=%.1f t=%.3fs depth=%.1f   ",
		       state_str(aim.c, alive), lx, lz, ddist, t, depth);
		fflush(stdout);
	}

	gtk_widget_queue_draw(aim.da);
	return TRUE;
}

static void run_aim(const struct cfg *c, unsigned long base) {
	memset(&aim, 0, sizeof(aim));
	aim.c = c;
	aim.base = base;

	/* find game window geometry from hyprctl */
	int gx = 0, gy = 0;
	FILE *hc = popen(
	    "hyprctl clients -j 2>/dev/null | "
	    "jq -r '.[] | select(.initialClass == \"Devil Daggers\") | "
	    "\"\\(.at[0]) \\(.at[1]) \\(.size[0]) \\(.size[1])\"'", "r");
	if (hc) {
		if (fscanf(hc, "%d %d %d %d", &gx, &gy, &aim.ww, &aim.wh) == 4)
			printf("game window: %dx%d at (%d,%d)\n", aim.ww, aim.wh, gx, gy);
		pclose(hc);
	}
	if (aim.ww == 0) { aim.ww = 1920; aim.wh = 1080; }

	/* GTK3 layer-shell overlay */
	gtk_init(NULL, NULL);

	aim.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_layer_init_for_window(GTK_WINDOW(aim.win));
	gtk_layer_set_layer(GTK_WINDOW(aim.win), GTK_LAYER_SHELL_LAYER_OVERLAY);
	gtk_layer_set_anchor(GTK_WINDOW(aim.win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
	gtk_layer_set_anchor(GTK_WINDOW(aim.win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
	gtk_layer_set_anchor(GTK_WINDOW(aim.win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
	gtk_layer_set_anchor(GTK_WINDOW(aim.win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
	gtk_layer_set_keyboard_interactivity(GTK_WINDOW(aim.win), FALSE);
	gtk_layer_set_exclusive_zone(GTK_WINDOW(aim.win), -1);

	/* pin to the monitor where the game is */
	GdkDisplay *gdk_dpy = gdk_display_get_default();
	int n = gdk_display_get_n_monitors(gdk_dpy);
	for (int i = 0; i < n; i++) {
		GdkMonitor *m = gdk_display_get_monitor(gdk_dpy, i);
		GdkRectangle geo;
		gdk_monitor_get_geometry(m, &geo);
		if (gx >= geo.x && gx < geo.x + geo.width &&
		    gy >= geo.y && gy < geo.y + geo.height) {
			gtk_layer_set_monitor(GTK_WINDOW(aim.win), m);
			printf("overlay on monitor %d (%d,%d %dx%d)\n",
			       i, geo.x, geo.y, geo.width, geo.height);
			break;
		}
	}

	/* transparent background */
	GdkScreen *screen = gdk_screen_get_default();
	GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
	if (visual) gtk_widget_set_visual(aim.win, visual);
	gtk_widget_set_app_paintable(aim.win, TRUE);

	aim.da = gtk_drawing_area_new();
	gtk_container_add(GTK_CONTAINER(aim.win), aim.da);
	g_signal_connect(aim.da, "draw", G_CALLBACK(aim_draw_cb), NULL);

	gtk_widget_show_all(aim.win);

	/* click-through: empty input region so all pointer events pass to game */
	GdkWindow *gdk_win = gtk_widget_get_window(aim.win);
	if (gdk_win) {
		cairo_region_t *empty = cairo_region_create();
		gdk_window_input_shape_combine_region(gdk_win, empty, 0, 0);
		cairo_region_destroy(empty);
	}

	printf("aimpoint overlay (layer-shell) — Ctrl-C to stop (fov=%.0f)\n\n",
	       c->fov > 0.0f ? c->fov : 90.0f);

	g_timeout_add(2, aim_tick_cb, NULL);
	gtk_main();
	printf("\n");
}

static void run_bhop(const struct cfg *c, unsigned long base, int strafe) {
	unsigned long hero=0;
	int jumps=0, log_tick=0;
	uint32_t prev_alive=0;
	int was_air=0;

	Display *dpy = XOpenDisplay(NULL);
	if(!dpy){fprintf(stderr,"no X display\n");return;}
	KeyCode space_kc = XKeysymToKeycode(dpy, XK_space);

	if(strafe) {
		logfile = fopen(LOG_PATH, "w");
		if(logfile) fprintf(logfile, "dd-bhop strafe log\n");
	}

	printf("bhop%s — hold space — Ctrl-C to stop\n\n", strafe?" +strafe":" (timing)");
	while(running){
		if(resolve(c,base,&hero)<0){usleep(POLL_US);continue;}
		uint32_t alive=ru(hero+c->h_alive);

		if(alive==0){
			if(jumps>0) printf("\n[DEAD] jumps=%d\n",jumps);
			jumps=0; was_air=0; prev_alive=0; usleep(10000); continue;
		}

		int air = (alive==c->state_air || alive==c->state_fall);
		int held = space_held(dpy, space_kc);

		if(held){
			wf(hero+c->h_timing, c->timing_val);
			wu(hero+c->h_perf, 1);

			if(strafe && air) {
				wf(hero+c->h_spd_a, c->boost_accel);
				wf(hero+c->h_spd_b, c->boost_accel);
				wf(hero+c->h_spd_c, c->boost_spd_c);
				wf(hero+c->h_spd_d, c->boost_spd_d);
				wf(hero+c->h_spd_e, c->boost_spd_e);
				wf(hero+c->h_frict, c->boost_frict);

				float py = rf(hero+c->h_pos_y);
				float vy = rf(hero+c->h_vel_y);
				if(py < c->max_boost_alt) {
					wf(hero+c->h_vel_y, vy + c->gravity_cut);
				}

				log_tick++;
				if(log_tick % 50 == 0) {
					float vx=rf(hero+c->h_vel_x), vz=rf(hero+c->h_vel_z);
					float speed=sqrtf(vx*vx+vz*vz);
					float yaw=rf(hero+c->h_yaw);
					LOG("[%d] state=%d hspd=%.2f py=%.2f vy=%.2f grav=%s yaw=%.2f\n",
					    log_tick, alive, speed, py, vy, py<c->max_boost_alt?"on":"off", yaw);
				}
			}
		}

		if(was_air && alive==c->state_air && prev_alive!=c->state_air)
			jumps++;

		was_air=air;
		prev_alive=alive;

		if(jumps%3==0||!air){
			float py=rf(hero+c->h_pos_y);
			float vx=rf(hero+c->h_vel_x), vz=rf(hero+c->h_vel_z);
			float hs=sqrtf(vx*vx+vz*vz);
			float sa=rf(hero+c->h_spd_a), fr=rf(hero+c->h_frict);
			printf("\r[bhop] py=%.3f %s jumps=%d hspd=%.1f accel=%.1f fr=%.4f%s   ",
			       py, state_str(c,alive), jumps, hs, sa, fr, strafe&&air?" [BOOST]":"");
			fflush(stdout);
		}
		usleep(POLL_US);
	}
	if(logfile) fclose(logfile);
	printf("\nstopped. jumps=%d\n",jumps);
	XCloseDisplay(dpy);
}

static void run_teleport(const struct cfg *c, unsigned long base, float tx, float tz) {
	unsigned long hero=0;
	if(resolve(c,base,&hero)<0){fprintf(stderr,"chain fail\n");return;}
	float ox=rf(hero+c->h_pos_x), oz=rf(hero+c->h_pos_z);
	wf(hero+c->h_pos_x, tx); wf(hero+c->h_pos_z, tz);
	printf("teleported (%.2f,%.2f) -> (%.2f,%.2f)\n",ox,oz,tx,tz);
}

static void usage(const char *p){
	fprintf(stderr,"dd-bhop — Devil Daggers bhop + air control\n"
	    "  %s              bhop only (hold space)\n"
	    "  %s --strafe     bhop + air control boost\n"
	    "  %s --diag       diagnostic\n"
	    "  %s --aim        dagger landing prediction\n"
	    "  %s --teleport X Z\n",p,p,p,p,p);
}

int main(int argc, char **argv){
	int mode=0, strafe=0; float tx=0,tz=0;
	for(int i=1;i<argc;i++){
		if(!strcmp(argv[i],"--diag")) mode=1;
		else if(!strcmp(argv[i],"--aim")) mode=3;
		else if(!strcmp(argv[i],"--strafe")) strafe=1;
		else if(!strcmp(argv[i],"--teleport")&&i+2<argc){mode=2;tx=strtof(argv[++i],NULL);tz=strtof(argv[++i],NULL);}
		else{usage(argv[0]);return 1;}
	}

	struct cfg c;
	memset(&c, 0, sizeof(c));

	/* aim mode requires h_pitch + physics params in config */

	/* try exe dir, then cwd, then ~/.config */
	int loaded = 0;
	char exe_path[512];
	ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path)-1);
	if (len > 0) {
		exe_path[len] = 0;
		char *slash = strrchr(exe_path, '/');
		if (slash) { strcpy(slash+1, "dd-bhop.conf"); if(load_config(exe_path,&c)==0) loaded=1; }
	}
	if (!loaded) { char *cwd = get_current_dir_name(); if(cwd){char p[512];snprintf(p,512,"%s/dd-bhop.conf",cwd);if(load_config(p,&c)==0)loaded=1;free(cwd);} }
	if (!loaded) { char p[256]; snprintf(p,256,"%s/.config/dd-bhop.conf",getenv("HOME")?:"/tmp"); if(load_config(p,&c)==0)loaded=1; }
	if (!loaded) { fprintf(stderr,"dd-bhop.conf not found. See example.conf.\n"); return 1; }

	FILE*pg=popen("pgrep -x devildaggers","r");
	if(!pg||fscanf(pg,"%d",&pid)!=1){fprintf(stderr,"not running\n");return 1;}pclose(pg);
	unsigned long base=get_base(); if(!base){fprintf(stderr,"no base\n");return 1;}
	printf("pid=%d base=0x%lx\n",pid,base);
	signal(SIGINT,sighandler); signal(SIGTERM,sighandler);
	switch(mode){
		case 0: run_bhop(&c,base,strafe); break;
		case 1: run_diag(&c,base); break;
		case 2: run_teleport(&c,base,tx,tz); break;
		case 3: run_aim(&c,base); break;
	}
	return 0;
}
