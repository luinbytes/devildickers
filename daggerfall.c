/*
 * daggerfall — bhop + air control for Devil Daggers
 *
 * Usage:
 *   daggerfall              — bhop only (hold space)
 *   daggerfall --strafe     — bhop + air control boost
 *   daggerfall --diag       — live diagnostic
 *   daggerfall --aim        — dagger landing prediction (read-only)
 *   daggerfall --teleport X Z
 *
 * Requires daggerfall.conf (see example.conf)
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
#include <glib-unix.h>

#define POLL_US  2000
#define LOG_PATH "/tmp/dd-strafe.log"

/* dagger entity struct offsets (0x88 bytes per dagger) */
#define DAG_SIZE  0x88
#define DAG_TYPE  0x00   /* uint32: 1-6=live level, 7=dead/removed */
#define DAG_SPEED 0x08   /* float:  travel speed (units/tick) */
#define DAG_DIR_X 0x24   /* float:  forward direction x */
#define DAG_DIR_Y 0x28   /* float:  forward direction y */
#define DAG_DIR_Z 0x2c   /* float:  forward direction z */
#define DAG_POS_X 0x30   /* float:  world position x */
#define DAG_POS_Y 0x34   /* float:  world position y */
#define DAG_POS_Z 0x38   /* float:  world position z */
#define DAG_UID   0x84   /* uint32: spawn id (higher = more recent) */


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
	float vel_scale;  /* player-velocity inheritance scalar (0=none, 1=full) */
	float squid_gem_y_add;  /* extra Y added to squid gem aim point (tune for visual body) */
	float aim_smooth;       /* hold-smoothing factor 0..1 (0.3 = ease to target) */
	float aim_flick;        /* per-click flick smoothing (1.0 = snap instantly) */
	int   aim_flick_frames; /* how many frames the flick smoothing stays active */
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

/* Required-key bitmask indices. load_config builds a per-mode required
 * mask: common movement/offset keys are needed by all modes, aim-physics
 * keys (h_pitch, dagger_*, fov) are only required when mode == 3 (aim). */
enum {
	K_OFF_GLOBALS, K_OFF_ARENA, K_OFF_HERO,
	K_H_POS_X, K_H_POS_Y, K_H_POS_Z,
	K_H_VEL_X, K_H_VEL_Y, K_H_VEL_Z,
	K_H_YAW, K_H_PITCH, K_H_ALIVE,
	K_H_SPD_A, K_H_SPD_B, K_H_SPD_C, K_H_SPD_D, K_H_SPD_E,
	K_H_FRICT, K_H_TIMING, K_H_PERF,
	K_STATE_GND, K_STATE_AIR, K_STATE_FALL,
	K_TIMING_VAL,
	K_BOOST_ACCEL, K_BOOST_SPD_C, K_BOOST_SPD_D, K_BOOST_SPD_E,
	K_BOOST_FRICT, K_GRAVITY_CUT, K_MAX_BOOST_ALT,
	K_DAGGER_SPEED, K_DAGGER_GRAVITY, K_DAGGER_UP,
	K_FOV,
	K_VEL_SCALE,   /* optional — tuning scalar for player-velocity inheritance */
	K_SQUID_GEM_Y_ADD,   /* optional — extra Y on squid gem aim point */
	K_AIM_SMOOTH,        /* optional — hold-smoothing factor 0..1 */
	K_AIM_FLICK,         /* optional — per-click flick factor 0..1 */
	K_AIM_FLICK_FRAMES,  /* optional — frames the flick lasts */
	K_COUNT
};
static const char *const REQ_NAMES[K_COUNT] = {
	"off_globals","off_arena","off_hero",
	"h_pos_x","h_pos_y","h_pos_z",
	"h_vel_x","h_vel_y","h_vel_z",
	"h_yaw","h_pitch","h_alive",
	"h_spd_a","h_spd_b","h_spd_c","h_spd_d","h_spd_e",
	"h_frict","h_timing","h_perf",
	"state_gnd","state_air","state_fall",
	"timing_val",
	"boost_accel","boost_spd_c","boost_spd_d","boost_spd_e",
	"boost_frict","gravity_cut","max_boost_alt",
	"dagger_speed","dagger_gravity","dagger_up",
	"fov",
	"vel_scale",
	"squid_gem_y_add",
	"aim_smooth",
	"aim_flick",
	"aim_flick_frames",
};

static int load_config(const char *path, struct cfg *c, int mode) {
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	char line[256];
	uint64_t seen = 0;   /* bit i set => REQ_NAMES[i] was loaded OK */
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
		int matched = 0;
		#define C(kidx, field, parse, T) \
			if(!matched && strcmp(k,REQ_NAMES[kidx])==0){T t;if(parse(v,&t)==0){c->field=t;seen|=(uint64_t)1<<(kidx);}else{fprintf(stderr,"config: bad value for '%s': %s\n",REQ_NAMES[kidx],v);fclose(f);return -1;}matched=1;}
		C(K_OFF_GLOBALS, off_globals, parse_ulong, unsigned long)
		C(K_OFF_ARENA,   off_arena,   parse_ulong, unsigned long)
		C(K_OFF_HERO,    off_hero,    parse_ulong, unsigned long)
		C(K_H_POS_X, h_pos_x, parse_ulong, unsigned long)
		C(K_H_POS_Y, h_pos_y, parse_ulong, unsigned long)
		C(K_H_POS_Z, h_pos_z, parse_ulong, unsigned long)
		C(K_H_VEL_X, h_vel_x, parse_ulong, unsigned long)
		C(K_H_VEL_Y, h_vel_y, parse_ulong, unsigned long)
		C(K_H_VEL_Z, h_vel_z, parse_ulong, unsigned long)
		C(K_H_YAW,   h_yaw,   parse_ulong, unsigned long)
		C(K_H_PITCH, h_pitch, parse_ulong, unsigned long)
		C(K_H_ALIVE, h_alive, parse_ulong, unsigned long)
		C(K_H_SPD_A, h_spd_a, parse_ulong, unsigned long)
		C(K_H_SPD_B, h_spd_b, parse_ulong, unsigned long)
		C(K_H_SPD_C, h_spd_c, parse_ulong, unsigned long)
		C(K_H_SPD_D, h_spd_d, parse_ulong, unsigned long)
		C(K_H_SPD_E, h_spd_e, parse_ulong, unsigned long)
		C(K_H_FRICT, h_frict, parse_ulong, unsigned long)
		C(K_H_TIMING,h_timing,parse_ulong, unsigned long)
		C(K_H_PERF,  h_perf,  parse_ulong, unsigned long)
		C(K_STATE_GND,  state_gnd,  parse_uint, uint32_t)
		C(K_STATE_AIR,  state_air,  parse_uint, uint32_t)
		C(K_STATE_FALL, state_fall, parse_uint, uint32_t)
		C(K_TIMING_VAL,  timing_val,  parse_float, float)
		C(K_BOOST_ACCEL, boost_accel, parse_float, float)
		C(K_BOOST_SPD_C, boost_spd_c, parse_float, float)
		C(K_BOOST_SPD_D, boost_spd_d, parse_float, float)
		C(K_BOOST_SPD_E, boost_spd_e, parse_float, float)
		C(K_BOOST_FRICT, boost_frict, parse_float, float)
		C(K_GRAVITY_CUT,   gravity_cut,   parse_float, float)
		C(K_MAX_BOOST_ALT, max_boost_alt, parse_float, float)
		C(K_DAGGER_SPEED,    dagger_speed,    parse_float, float)
		C(K_DAGGER_GRAVITY,  dagger_gravity,  parse_float, float)
		C(K_DAGGER_UP,       dagger_up,       parse_float, float)
		C(K_FOV,             fov,             parse_float, float)
		C(K_VEL_SCALE,       vel_scale,       parse_float, float)
		C(K_SQUID_GEM_Y_ADD, squid_gem_y_add, parse_float, float)
		C(K_AIM_SMOOTH,      aim_smooth,      parse_float, float)
		C(K_AIM_FLICK,       aim_flick,       parse_float, float)
		#define PARSE_INT(s, o) parse_ulong((s), (unsigned long*)(o))
		if(!matched && strcmp(k,REQ_NAMES[K_AIM_FLICK_FRAMES])==0){
			unsigned long t;
			if(parse_ulong(v,&t)==0){c->aim_flick_frames=(int)t;seen|=(uint64_t)1<<(K_AIM_FLICK_FRAMES);}
			else{fprintf(stderr,"config: bad value for '%s': %s\n",REQ_NAMES[K_AIM_FLICK_FRAMES],v);fclose(f);return -1;}
			matched=1;
		}
		#undef PARSE_INT
		#undef C
		if (!matched && *k)
			fprintf(stderr, "config: warning: unknown key '%s' (ignored)\n", k);
	}
	fclose(f);

	/* Build the required-key mask for this mode. Common keys are
	 * required by every mode; aim-physics keys only by aim (mode==3). */
	if (!(seen & ((uint64_t)1 << K_VEL_SCALE))) c->vel_scale = 0.5f;
	if (!(seen & ((uint64_t)1 << K_SQUID_GEM_Y_ADD))) c->squid_gem_y_add = 0.0f;
	if (!(seen & ((uint64_t)1 << K_AIM_SMOOTH)))      c->aim_smooth = 0.30f;
	if (!(seen & ((uint64_t)1 << K_AIM_FLICK)))       c->aim_flick  = 1.00f;
	if (!(seen & ((uint64_t)1 << K_AIM_FLICK_FRAMES)))c->aim_flick_frames = 3;

	uint64_t req_all = (K_COUNT < 64) ? (((uint64_t)1 << K_COUNT) - 1) : ~(uint64_t)0;
	uint64_t aim_only = ((uint64_t)1 << K_H_PITCH)
	                  | ((uint64_t)1 << K_DAGGER_SPEED)
	                  | ((uint64_t)1 << K_DAGGER_GRAVITY)
	                  | ((uint64_t)1 << K_DAGGER_UP)
	                  | ((uint64_t)1 << K_FOV);
	uint64_t optional = ((uint64_t)1 << K_VEL_SCALE)
	                  | ((uint64_t)1 << K_DAGGER_GRAVITY)
	                  | ((uint64_t)1 << K_SQUID_GEM_Y_ADD)
	                  | ((uint64_t)1 << K_AIM_SMOOTH)
	                  | ((uint64_t)1 << K_AIM_FLICK)
	                  | ((uint64_t)1 << K_AIM_FLICK_FRAMES);
	uint64_t required = req_all & ~optional;
	if (mode != 3) required &= ~aim_only;

	uint64_t missing = required & ~seen;
	if (missing) {
		for (int i = 0; i < K_COUNT; i++)
			if (missing & ((uint64_t)1 << i))
				fprintf(stderr, "config: missing required key '%s'\n", REQ_NAMES[i]);
		return -1;
	}
	return 0;
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
	/* resolve the actual executable path so we match only the game binary,
	 * not random files with "devildaggers" in the name (log files, etc). */
	char exe_link[64]; snprintf(exe_link,64,"/proc/%d/exe",pid);
	char exe_path[4096];
	ssize_t el = readlink(exe_link, exe_path, sizeof(exe_path)-1);
	if (el <= 0) return 0;
	exe_path[el] = 0;
	const char *exe_base = strrchr(exe_path, '/');
	exe_base = exe_base ? exe_base + 1 : exe_path;
	if (!*exe_base) return 0;

	char p[64]; snprintf(p,64,"/proc/%d/maps",pid);
	FILE *f=fopen(p,"r"); if(!f) return 0;
	char line[512]; unsigned long b=0;
	size_t n = strlen(exe_base);
	while(fgets(line,sizeof(line),f)) {
		if (!strstr(line,"r-xp")) continue;
		/* maps lines end with the pathname; require the basename to be
		 * preceded by '/' AND followed by end-of-pathname so we only
		 * match a full path component, not a substring of some unrelated
		 * path. The basename may appear multiple times in one line (e.g.
		 * when the containing directory is also named the same), so walk
		 * every occurrence until one passes the boundary check. */
		for (char *hit = line; (hit = strstr(hit, exe_base)) != NULL; hit++) {
			if (hit > line && *(hit-1) == '/' &&
			    (hit[n] == '\0' || hit[n] == '\n' || hit[n] == '\r')) {
				b = strtoul(line, NULL, 16);
				goto done;
			}
		}
	}
done:
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
		float sa=rf(hero+c->h_spd_a), sb=rf(hero+c->h_spd_b), fr=rf(hero+c->h_frict);
		float dx=(cx-px)/dt, dz=(cz-pz)/dt, hs=sqrtf(dx*dx+dz*dz);
		printf("\r[%s] pos=(%7.2f,%5.2f,%7.2f) vel=(%6.1f,%6.1f,%6.1f) hspd=%5.1f spd=(%.1f,%.1f) fr=%.4f  ",
		       state_str(c,alive), cx,cy,cz,vx,vy,vz,hs,sa,sb,fr);
		fflush(stdout);
		px=cx; pz=cz; prev=now; usleep(5000);
	}
	printf("\n");
}

/* per-tick state for bhop loop — persists across bhop_step() calls so the
 * same state can be driven either by run_bhop's while-loop or by a GTK
 * timer installed inside run_aim. */
struct bhop_state {
	int jumps;
	int log_tick;
	uint32_t prev_alive;
	int was_air;
	Display *dpy;
	KeyCode space_kc;
};

/* forward decl so bhop_gtk_cb (defined near run_aim, before bhop_step's
 * body) can call the step function. */
static int bhop_step(const struct cfg *c, unsigned long base, int strafe,
                     struct bhop_state *st);

/* aim overlay context */
static struct {
	const struct cfg *c;
	unsigned long base;
	int ww, wh;
	float mx, my;
	float src_x, src_y;   /* screen-space trajectory source */
	int have_src;         /* 1 when src_x/src_y are valid */
	int vis, tick;
	int armed;            /* 1 when LMB held (aim assist engaged) */
	float tgt_x, tgt_y, tgt_z;     /* last targeted world position (diag) */
	float last_target_pitch;       /* last computed target pitch (diag) */
	GtkWidget *win, *da;
	Display *key_dpy;
} aim;

/* bhop timer context for when --aim composes with --strafe. populated by
 * run_aim before g_timeout_add, consumed by bhop_gtk_cb. */
static struct bhop_state bhop_st;
static struct {
	const struct cfg *c;
	unsigned long base;
	int strafe;
} bhop_arg;

/* Sticky target max lateral drift per tick (world units) — lead prediction
 * can move the camera so far off the actual enemy that a fresh cone search
 * loses them. As long as an enemy exists within this radius of last tick's
 * target position AND is in front of the player, we keep tracking it. */
#define STICKY_RADIUS 8.0f

static int find_closest_enemy_fov(unsigned long arena,
                                   float cam_x, float cam_y, float cam_z,
                                   float fwd_x, float fwd_y, float fwd_z,
                                   /* player-camera forward (real, not cone axis).
                                    * Used for the sticky "is this in front of
                                    * the player" filter so a mis-pointed cone
                                    * axis can't lock us behind the player. */
                                   float player_fwd_x, float player_fwd_y, float player_fwd_z,
                                   float half_fov_deg, float max_dist,
                                   float c_cfg_gem_y_add,
                                   int   have_sticky,
                                   float sx, float sy, float sz,
                                   float *out_x, float *out_y, float *out_z) {
	/* acquisition scoring: closest-to-camera-forward inside acquire cone */
	float best_angle = half_fov_deg;
	float best_dist = max_dist;
	float bx = 0.0f, by = 0.0f, bz = 0.0f;
	int found = 0;

	/* sticky scoring: closest-to-previous-target-position (spatial) */
	float sticky_best_d2 = STICKY_RADIUS * STICKY_RADIUS;
	float sbx = 0.0f, sby = 0.0f, sbz = 0.0f;
	int sticky_found = 0;

	#define CONSIDER(ex, ey, ez) do { \
		float dx = (ex) - cam_x, dy = (ey) - cam_y, dz = (ez) - cam_z; \
		float dist = sqrtf(dx*dx + dy*dy + dz*dz); \
		if (dist >= 0.5f && dist <= max_dist) { \
			float inv = 1.0f / dist; \
			float nx = dx * inv, ny = dy * inv, nz = dz * inv; \
			float dot = nx*fwd_x + ny*fwd_y + nz*fwd_z; \
			if (dot > 1.0f) dot = 1.0f; \
			else if (dot < -1.0f) dot = -1.0f; \
			/* sticky: keep target as long as it's within ~120° of player \
			 * forward. > 0 would drop overhead enemies (skull floated up, \
			 * pdot ≈ 0 → sticky lost → cone snaps to floor enemy). -0.5 \
			 * gives headroom for directly-overhead (90°, pdot=0) without \
			 * letting truly-behind targets (>120°) sneak through. */ \
			if (have_sticky) { \
				float pdot = nx*player_fwd_x + ny*player_fwd_y + nz*player_fwd_z; \
				if (pdot > -0.5f) { \
					float sdx = (ex) - sx, sdy = (ey) - sy, sdz = (ez) - sz; \
					float sd2 = sdx*sdx + sdy*sdy + sdz*sdz; \
					/* Same-tier height filter: don't let sticky grab a \
					 * skull on the floor (Y≈0.5) when the last target \
					 * was a squid (Y≈6). Real targets don't warp 5 \
					 * units up/down in one tick. */ \
					if (sd2 < sticky_best_d2 && fabsf(sdy) < 3.0f) { \
						sticky_best_d2 = sd2; \
						sbx = (ex); sby = (ey); sbz = (ez); \
						sticky_found = 1; \
					} \
				} \
			} \
			float angle_deg = acosf(dot) * (180.0f / (float)M_PI); \
			if (angle_deg <= half_fov_deg) { \
				if (!found || angle_deg < best_angle || \
				    (angle_deg == best_angle && dist < best_dist)) { \
					best_angle = angle_deg; \
					best_dist = dist; \
					bx = (ex); by = (ey); bz = (ez); \
					found = 1; \
				} \
			} \
		} \
	} while (0)

	unsigned long boid_god = rp(arena + 0x318);
	if (boid_god >= 0x10000) {
		unsigned long b0 = rp(boid_god + 0x20);
		unsigned long b1 = rp(boid_god + 0x28);
		if (b0 && b1 > b0) {
			size_t bn = (b1 - b0) / 0xf0;
			if (bn > 512) bn = 512;
			for (size_t i = 0; i < bn; i++) {
				unsigned long b = b0 + i * 0xf0;
				uint32_t tw = ru(b);
				uint16_t btype   = (uint16_t)(tw & 0xffff);
				int16_t  bhealth = (int16_t)(tw >> 16);
				if (btype == 0 || btype > 5 || bhealth <= 0) continue;
				float x = rf(b + 0x08), y = rf(b + 0x0c), z = rf(b + 0x10);
				CONSIDER(x, y, z);
			}
		}
	}

	unsigned long sq_sys = rp(arena + 0x330);
	if (sq_sys >= 0x10000) {
		unsigned long sq0 = rp(sq_sys + 0x18);
		unsigned long sq1 = rp(sq_sys + 0x20);
		if (sq0 && sq1 > sq0) {
			size_t n = (sq1 - sq0) / 0x148;
			if (n > 512) n = 512;
			for (size_t i = 0; i < n; i++) {
				unsigned long sq = sq0 + i * 0x148;
				if (ru(sq + 0x00) >= 2) continue;  /* dying/dead */
				uint32_t variant = ru(sq + 0xf0);
				float bpx = rf(sq + 0x0c), bpy = rf(sq + 0x10), bpz = rf(sq + 0x14);
				float scale = rf(sq + 0x8c);
				/* col0 of the squid's rotation matrix (+0x18/1c/20): the gems
				 * orbit along ±col0 at radius 0.59*scale. col0.y is ~0 because
				 * the final orientation rotates only in the XZ plane. */
				float cx = rf(sq + 0x18), cy = rf(sq + 0x1c), cz = rf(sq + 0x20);
				float radial = 0.59f * scale;  /* DAT_003af130 = 0.59 */
				/* zone hp counters: +0x94 = gem0 (on +col0), +0x98 = gem1 (-col0).
				 * zone <= 0 = that gem has been broken off, skip it. */
				int z0_alive = (int)ru(sq + 0x94) > 0;
				int z1_alive = (int)ru(sq + 0x98) > 0;
				float y_add = c_cfg_gem_y_add;  /* filled below */
				(void)y_add;  /* silence if accidentally unused */

				if (variant == 2) {
					/* Squid III (centipede-style): 3 zones, radius = 1.6*scale.
					 * Body position is the best single target we have without
					 * per-segment data; keep it simple. */
					CONSIDER(bpx, bpy + c_cfg_gem_y_add, bpz);
					continue;
				}

				/* Squid I/II (variant 0/1): two gems on opposite sides.
				 * Visibility = gem side dot (player - body) > 0 in XZ. */
				float to_px = cam_x - bpx;
				float to_pz = cam_z - bpz;
				float face0 = cx * to_px + cz * to_pz;  /* + = gem0 on near side */

				if (z0_alive && face0 > 0.0f) {
					float gx = bpx + radial * cx;
					float gy = bpy + radial * cy + c_cfg_gem_y_add;
					float gz = bpz + radial * cz;
					CONSIDER(gx, gy, gz);
				}
				if (z1_alive && face0 < 0.0f) {
					float gx = bpx - radial * cx;
					float gy = bpy - radial * cy + c_cfg_gem_y_add;
					float gz = bpz - radial * cz;
					CONSIDER(gx, gy, gz);
				}
				/* fallback when neither gem is visible (back-facing angle,
				 * or both dead) — aim at body center so we still have a
				 * target to track for movement prediction. */
				if (!((z0_alive && face0 > 0.0f) || (z1_alive && face0 < 0.0f))) {
					CONSIDER(bpx, bpy + c_cfg_gem_y_add, bpz);
				}
			}
		}
	}

	unsigned long sp_sys = rp(arena + 0x340);
	if (sp_sys >= 0x10000) {
		unsigned long sp0 = rp(sp_sys + 0x28);
		unsigned long sp1 = rp(sp_sys + 0x30);
		if (sp0 && sp1 > sp0) {
			size_t n = (sp1 - sp0) / 0x160;
			if (n > 512) n = 512;
			for (size_t i = 0; i < n; i++) {
				unsigned long sp = sp0 + i * 0x160;
				if (ru(sp + 0x00) >= 3) continue;
				float scale = rf(sp + 0xac);
				float wx = rf(sp + 0x24) + 1.6f * scale * rf(sp + 0x48);
				float wy = rf(sp + 0x28) + 1.6f * scale * rf(sp + 0x4c);
				float wz = rf(sp + 0x2c) + 1.6f * scale * rf(sp + 0x50);
				CONSIDER(wx, wy, wz);
			}
		}
	}

	unsigned long egg0 = rp(arena + 0x290);
	unsigned long egg1 = rp(arena + 0x298);
	if (egg0 && egg1 > egg0) {
		size_t n = (egg1 - egg0) / 0xa0;
		if (n > 512) n = 512;
		for (size_t i = 0; i < n; i++) {
			unsigned long e = egg0 + i * 0xa0;
			if (ru(e + 0x84) != 0) continue;
			float x = rf(e + 0x34), y = rf(e + 0x38), z = rf(e + 0x3c);
			CONSIDER(x, y, z);
		}
	}

	#undef CONSIDER

	/* Sticky wins: same enemy as last tick beats the cone-closest candidate.
	 * If the sticky match fails (enemy died, turned away, etc.), fall back
	 * to fresh acquisition inside the cone. */
	if (sticky_found) {
		*out_x = sbx; *out_y = sby; *out_z = sbz;
		return 1;
	}
	if (!found) return 0;
	*out_x = bx; *out_y = by; *out_z = bz;
	return 1;
}

/* scan arena dagger pool for the live dagger with the highest uid.
 * returns 1 and fills out params if found, 0 if pool is empty or all dead.
 * Currently unused — the overlay is PRED-only to avoid jitter from the
 * highest-UID dagger changing every shot — but kept for future use. */
__attribute__((unused))
static int read_latest_dagger(unsigned long arena,
                               float *pos_x, float *pos_y, float *pos_z,
                               float *dir_x, float *dir_y, float *dir_z,
                               float *speed) {
	unsigned long start = rp(arena + 0x1a0);
	unsigned long end   = rp(arena + 0x1a8);
	if (!start || !end || end <= start) return 0;
	size_t count = (end - start) / DAG_SIZE;
	if (count > 256) count = 256;

	uint32_t best_uid = 0;
	unsigned long best_addr = 0;
	for (size_t i = 0; i < count; i++) {
		unsigned long addr = start + i * DAG_SIZE;
		uint32_t type = ru(addr + DAG_TYPE);
		if (type == 0 || type == 7) continue;
		uint32_t uid = ru(addr + DAG_UID);
		if (!best_addr || uid > best_uid) {
			best_uid = uid;
			best_addr = addr;
		}
	}
	if (!best_addr) return 0;

	*pos_x = rf(best_addr + DAG_POS_X);
	*pos_y = rf(best_addr + DAG_POS_Y);
	*pos_z = rf(best_addr + DAG_POS_Z);
	*dir_x = rf(best_addr + DAG_DIR_X);
	*dir_y = rf(best_addr + DAG_DIR_Y);
	*dir_z = rf(best_addr + DAG_DIR_Z);
	*speed = rf(best_addr + DAG_SPEED);
	return 1;
}

static gboolean aim_draw_cb(GtkWidget *w, cairo_t *cr, gpointer p) {
	(void)w; (void)p;
	if (!aim.vis) return FALSE;
	/* clear to transparent */
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);
	/* trajectory line from source (camera centre or live dagger) to aimpoint */
	if (aim.have_src) {
		cairo_set_source_rgba(cr, 1.0, 0.62, 0.71, 0.45);
		cairo_set_line_width(cr, 1.5);
		cairo_move_to(cr, aim.src_x, aim.src_y);
		cairo_line_to(cr, aim.mx, aim.my);
		cairo_stroke(cr);
	}
	/* ring — dimmer when idle, full pink when LMB held and engaged */
	if (aim.armed)
		cairo_set_source_rgba(cr, 1.0, 0.62, 0.71, 0.95);
	else
		cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.35);
	cairo_set_line_width(cr, 2.0);
	cairo_arc(cr, aim.mx, aim.my, 10, 0, 2 * M_PI);
	cairo_stroke(cr);
	/* center dot */
	if (aim.armed)
		cairo_set_source_rgba(cr, 1.0, 0.62, 0.71, 1.0);
	else
		cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.55);
	cairo_arc(cr, aim.mx, aim.my, 3, 0, 2 * M_PI);
	cairo_fill(cr);
	return FALSE;
}

static gboolean aim_tick_cb(gpointer p) {
	(void)p;
	if (!running) { gtk_main_quit(); return FALSE; }

	/* 1. Resolve hero */
	unsigned long hero = 0;
	if (resolve(aim.c, aim.base, &hero) < 0) {
		aim.vis = 0;
		gtk_widget_queue_draw(aim.da);
		return TRUE;
	}

	/* 2. Alive check — hide during death / menu */
	uint32_t alive = ru(hero + aim.c->h_alive);
	if (alive != aim.c->state_gnd &&
	    alive != aim.c->state_air &&
	    alive != aim.c->state_fall) {
		aim.vis = 0;
		gtk_widget_queue_draw(aim.da);
		return TRUE;
	}

	/* 3. Position + eye height
	 * NOTE: previous builds also read hero+0xd0 as a "dynamic eye" value,
	 * but that field is unidentified and can produce garbage. Use the
	 * explicit dagger_up offset from config so the user can tune it. */
	float px = rf(hero + aim.c->h_pos_x);
	float py = rf(hero + aim.c->h_pos_y);
	float pz = rf(hero + aim.c->h_pos_z);
	float eye_y = py + aim.c->dagger_up;

	/* 4. Camera basis from yaw/pitch */
	float yaw   = rf(hero + aim.c->h_yaw);
	float pitch = rf(hero + aim.c->h_pitch);
	float cp = cosf(pitch), sp = sinf(pitch);
	float cy = cosf(yaw),   sy = sinf(yaw);
	float fx = sy*cp, fy = -sp, fz = -cy*cp;
	float rx = cy,    ry = 0.0f, rz = sy;
	float ux = ry*fz - rz*fy;
	float uy = rz*fx - rx*fz;
	float uz = rx*fy - ry*fx;

	/* 5. Landing point — SINGLE path along current aim direction.
	 *    Immutable below this block. Aim assist (step 8) never touches
	 *    lx/land_y/lz; it only rotates the camera, which feeds the
	 *    NEXT tick's fx/fy/fz. */
	float spd = aim.c->dagger_speed;
	float t;
	float land_y;
	if (fy < -0.005f && eye_y > 0.01f) {
		t = eye_y / (spd * -fy);
		if (t > 25.0f) t = 25.0f;
		land_y = 0.0f;
	} else {
		t = 25.0f;
		land_y = eye_y + spd * fy * t;
	}
	float lx = px + spd * fx * t;
	float lz = pz + spd * fz * t;

	/* 6. World-to-screen of the landing point */
	float dx = lx - px, dy = land_y - eye_y, dz = lz - pz;
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
		aim.vis = (aim.mx >= 0 && aim.mx <= (float)aim.ww &&
		           aim.my >= 0 && aim.my <= (float)aim.wh) ? 1 : 0;
	} else {
		aim.vis = 0;
	}

	/* 7. Trajectory source — always screen centre */
	aim.src_x = (float)aim.ww * 0.5f;
	aim.src_y = (float)aim.wh * 0.5f;
	aim.have_src = 1;

	/* 8. Aim assist — INDEPENDENT. Writes yaw/pitch only. Never modifies
	 *    lx/land_y/lz/aim.mx/aim.my. The visual above is already frozen
	 *    for this tick. Camera rotation takes effect next tick.
	 *
	 *    Movement prediction: track enemy delta-position between ticks
	 *    (16ms cadence) to estimate velocity, then lead the aim by the
	 *    dagger's expected flight time to the target. */
	{
		static float prev_ex = 0, prev_ey = 0, prev_ez = 0;
		static int   have_prev_target = 0;
		static int   lmb_prev = 0;
		static int   flick_frames_left = 0;
		/* previous-tick desired aim direction (unit vec from player through the
		 * leaded target). When valid, we use this as the FOV-cone centre
		 * instead of the hero's current camera-forward — this keeps the cone
		 * where we WANT to be looking rather than where the camera is lagging
		 * to due to smoothing + lead prediction. */
		static float prev_aim_dx = 0, prev_aim_dy = 0, prev_aim_dz = 0;
		static int   have_prev_aim_dir = 0;

		/* LMB gate: engage only while LMB is held. On press-edge, queue a
		 * few frames of hard-snap ("flick") so a quick tap — the shotgun
		 * release — still lands on the target. Polling root-window mask
		 * works regardless of which window has focus. */
		int lmb_now = 0;
		if (aim.key_dpy) {
			Window rr, cr_;
			int rx_, ry_, wx_, wy_;
			unsigned int mask;
			if (XQueryPointer(aim.key_dpy, DefaultRootWindow(aim.key_dpy),
			                  &rr, &cr_, &rx_, &ry_, &wx_, &wy_, &mask)) {
				lmb_now = (mask & Button1Mask) ? 1 : 0;
			}
		}
		if (lmb_now && !lmb_prev) flick_frames_left = aim.c->aim_flick_frames;
		lmb_prev = lmb_now;

		/* Engage while LMB is held, OR for a few frames after a tap so the
		 * shotgun release still snaps onto the target even if LMB was let
		 * go before the next tick landed. */
		int engage = lmb_now || flick_frames_left > 0;

		/* reticle: pink when engaged, dim white otherwise */
		aim.armed = engage ? 1 : 0;

		if (!engage) {
			have_prev_target  = 0;
			have_prev_aim_dir = 0;
			goto end_aim_assist;
		}

		unsigned long g     = rp(aim.base + aim.c->off_globals);
		unsigned long arena = rp(g + aim.c->off_arena);
		if (arena >= 0x10000) {
			/* Cone axis: prefer last tick's desired aim direction (where
			 * we wanted to be looking) over the actual lagging camera
			 * forward. BUT reject the saved direction if it's wandered
			 * more than ~45° off the real camera — that's a stale
			 * pointer from a prior target that's likely sending the
			 * cone where the player isn't looking. */
			float cone_fx = fx, cone_fy = fy, cone_fz = fz;
			if (have_prev_aim_dir) {
				float d = fx*prev_aim_dx + fy*prev_aim_dy + fz*prev_aim_dz;
				if (d > 0.707f) {  /* within ~45° of real forward */
					cone_fx = prev_aim_dx;
					cone_fy = prev_aim_dy;
					cone_fz = prev_aim_dz;
				} else {
					have_prev_aim_dir = 0;  /* stale, drop it */
				}
			}
			float ex, ey, ez;
			if (find_closest_enemy_fov(arena, px, eye_y, pz,
			                           cone_fx, cone_fy, cone_fz,
			                           fx, fy, fz,  /* real player forward */
			                           15.0f, 80.0f,
			                           aim.c->squid_gem_y_add,
			                           have_prev_target,
			                           prev_ex, prev_ey, prev_ez,
			                           &ex, &ey, &ez)) {
				/* velocity from previous tick (reset if target jumped > 10 units) */
				float vel_x = 0, vel_y = 0, vel_z = 0;
				if (have_prev_target) {
					float d2 = (ex-prev_ex)*(ex-prev_ex)
					         + (ey-prev_ey)*(ey-prev_ey)
					         + (ez-prev_ez)*(ez-prev_ez);
					if (d2 < 100.0f) {  /* < 10 units = same target */
						float inv_dt = 62.5f;  /* 16ms tick → 62.5 Hz */
						vel_x = (ex - prev_ex) * inv_dt;
						vel_y = (ey - prev_ey) * inv_dt;
						vel_z = (ez - prev_ez) * inv_dt;
					}
				}
				prev_ex = ex; prev_ey = ey; prev_ez = ez;
				have_prev_target = 1;
				aim.tgt_x = ex; aim.tgt_y = ey; aim.tgt_z = ez;

				/* lead: dagger flight time = distance / speed */
				float tdx0 = ex - px, tdy0 = ey - eye_y, tdz0 = ez - pz;
				float dist = sqrtf(tdx0*tdx0 + tdy0*tdy0 + tdz0*tdz0);
				float flight_t = dist / aim.c->dagger_speed;
				if (flight_t > 2.0f) flight_t = 2.0f;

				/* clamp velocity magnitude to reject spikes from target
				 * switches / teleporting enemies. 15 u/s is well above
				 * any real in-game motion. */
				float vmag = sqrtf(vel_x*vel_x + vel_y*vel_y + vel_z*vel_z);
				if (vmag > 15.0f) {
					float s = 15.0f / vmag;
					vel_x *= s; vel_y *= s; vel_z *= s;
				}

				/* Never lead downward. Enemies can't go below the floor,
				 * so any negative vel_y is either bobbing noise, dying-
				 * animation descent, or the game clamping against the
				 * floor next tick — leading along it only gives us floor
				 * shots. Horizontal (XZ) lead is unaffected. */
				if (vel_y < 0.0f) vel_y = 0.0f;

				float pred_x = ex + vel_x * flight_t;
				float pred_y = ey + vel_y * flight_t;
				float pred_z = ez + vel_z * flight_t;

				/* Hard floor safety: never aim below Y=0.5 regardless of
				 * what the target is doing. The floor is at Y=0 and no
				 * hittable enemy has a hitbox below ~0.5. */
				if (pred_y < 0.5f) pred_y = 0.5f;
				/* Also never drop below the enemy's own Y — pairs with
				 * the vel_y >= 0 rule above to make downward lead a
				 * no-op, but guards against any other source of drift. */
				if (pred_y < ey) pred_y = ey;

				float tdx = pred_x - px;
				float tdy = pred_y - eye_y;
				float tdz = pred_z - pz;
				float horiz = sqrtf(tdx*tdx + tdz*tdz);

				/* Remember where we WANT to look — next tick we use this
				 * as the cone axis so the FOV tracks the lead, not the
				 * lagging camera. */
				float adist = sqrtf(tdx*tdx + tdy*tdy + tdz*tdz);
				if (adist > 0.001f) {
					float ainv = 1.0f / adist;
					prev_aim_dx = tdx * ainv;
					prev_aim_dy = tdy * ainv;
					prev_aim_dz = tdz * ainv;
					have_prev_aim_dir = 1;
				}

				float target_yaw   = atan2f(tdx, -tdz);
				float target_pitch = atan2f(-tdy, horiz);
				aim.last_target_pitch = target_pitch;

				float dyaw = target_yaw - yaw;
				while (dyaw >  (float)M_PI) dyaw -= 2.0f * (float)M_PI;
				while (dyaw < -(float)M_PI) dyaw += 2.0f * (float)M_PI;

				float smooth = aim.c->aim_smooth;
				if (flick_frames_left > 0) {
					smooth = aim.c->aim_flick;
					flick_frames_left--;
				}
				wf(hero + aim.c->h_yaw,   yaw   + dyaw * smooth);
				wf(hero + aim.c->h_pitch, pitch + (target_pitch - pitch) * smooth);
			} else {
				have_prev_target  = 0;
				have_prev_aim_dir = 0;
			}
		}
		end_aim_assist:;
	}

	/* 9. Diagnostic */
	if (++aim.tick % 10 == 0) {
		float ddist = sqrtf((lx - px)*(lx - px) + (lz - pz)*(lz - pz));
		printf("\r[%s] armed=%d  py=%.2f eye_y=%.2f  "
		       "tgt=(%.1f,%.2f,%.1f)  pitch=%.2f->%.2f  "
		       "aim=(%.1f,%.1f) dist=%.1f  ",
		       state_str(aim.c, alive), aim.armed,
		       py, eye_y,
		       aim.tgt_x, aim.tgt_y, aim.tgt_z,
		       pitch, aim.last_target_pitch,
		       lx, lz, ddist);
		fflush(stdout);
	}

	/* periodic re-query of game window geometry so the overlay follows
	 * the game if the user drags it to another monitor or resizes it.
	 * runs every ~60 ticks (~1s at 60Hz). if the game window can't be
	 * found we skip silently — don't destroy the overlay. */
	static int tick_counter = 0;
	static int cached_gx = 0, cached_gy = 0;
	static int have_cache = 0;
	if (++tick_counter >= 60) {
		tick_counter = 0;
		int new_gx = 0, new_gy = 0, new_ww = 0, new_wh = 0;
		FILE *hc = popen(
		    "hyprctl clients -j 2>/dev/null | "
		    "jq -r '.[] | select(.initialClass == \"Devil Daggers\") | "
		    "\"\\(.at[0]) \\(.at[1]) \\(.size[0]) \\(.size[1])\"'", "r");
		int matched = 0;
		if (hc) {
			matched = (fscanf(hc, "%d %d %d %d",
			                  &new_gx, &new_gy, &new_ww, &new_wh) == 4);
			pclose(hc);
		}
		if (matched && new_ww > 0 && new_wh > 0) {
			aim.ww = new_ww;
			aim.wh = new_wh;
			if (!have_cache || new_gx != cached_gx || new_gy != cached_gy) {
				GdkDisplay *gdk_dpy = gdk_display_get_default();
				int nmon = gdk_display_get_n_monitors(gdk_dpy);
				for (int i = 0; i < nmon; i++) {
					GdkMonitor *m = gdk_display_get_monitor(gdk_dpy, i);
					GdkRectangle geo;
					gdk_monitor_get_geometry(m, &geo);
					if (new_gx >= geo.x && new_gx < geo.x + geo.width &&
					    new_gy >= geo.y && new_gy < geo.y + geo.height) {
						gtk_layer_set_monitor(GTK_WINDOW(aim.win), m);
						break;
					}
				}
				cached_gx = new_gx;
				cached_gy = new_gy;
				have_cache = 1;
			}
		}
	}

	gtk_widget_queue_draw(aim.da);
	return TRUE;
}

static gboolean aim_sigint_cb(gpointer user_data) {
	(void)user_data;
	running = 0;
	gtk_main_quit();
	return G_SOURCE_REMOVE;
}

/* GTK timer driving the bhop/strafe loop inside run_aim when the user
 * passes --strafe --aim. Fires every 2ms (matches POLL_US in run_bhop). */
static gboolean bhop_gtk_cb(gpointer user_data) {
	(void)user_data;
	if (!running) return G_SOURCE_REMOVE;
	bhop_step(bhop_arg.c, bhop_arg.base, bhop_arg.strafe, &bhop_st);
	return G_SOURCE_CONTINUE;
}

static void run_aim(const struct cfg *c, unsigned long base, int strafe) {
	memset(&aim, 0, sizeof(aim));
	aim.c = c;
	aim.base = base;

	/* open X display (reserved for future hotkeys) */
	aim.key_dpy = XOpenDisplay(NULL);

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
	g_unix_signal_add(SIGINT, aim_sigint_cb, NULL);

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

	g_timeout_add(16, aim_tick_cb, NULL);

	/* Compose with --strafe: piggy-back a second GTK timer that drives
	 * the exact same bhop_step() the CLI-only bhop loop uses. Needs its
	 * own X display connection for XQueryKeymap (space_held). */
	if (strafe) {
		memset(&bhop_st, 0, sizeof bhop_st);
		bhop_st.dpy = XOpenDisplay(NULL);
		if (!bhop_st.dpy) {
			fprintf(stderr, "strafe: no X display — aim only\n");
		} else {
			bhop_st.space_kc = XKeysymToKeycode(bhop_st.dpy, XK_space);
			bhop_arg.c = c;
			bhop_arg.base = base;
			bhop_arg.strafe = 1;
			logfile = fopen(LOG_PATH, "w");
			if (logfile) fprintf(logfile, "daggerfall strafe log (composed with --aim)\n");
			printf("strafe timer armed — hold space to boost\n");
			g_timeout_add(2, bhop_gtk_cb, NULL);
		}
	}

	gtk_main();
	printf("\n");

	if (aim.key_dpy) { XCloseDisplay(aim.key_dpy); aim.key_dpy = NULL; }
	if (bhop_st.dpy) {
		if (logfile) { fclose(logfile); logfile = NULL; }
		XCloseDisplay(bhop_st.dpy);
		bhop_st.dpy = NULL;
	}
}

/* bhop_step return codes — let run_bhop preserve the exact sleep cadence
 * of the pre-refactor loop. The GTK-driven caller ignores the return value. */
#define BHOP_OK          0   /* normal tick — caller sleeps POLL_US (2ms) */
#define BHOP_RESOLVEFAIL 1   /* chain unresolved this tick — original slept POLL_US */
#define BHOP_DEAD        2   /* alive==0 (menu/death) — original slept 10ms */

/* One tick of the bhop/strafe loop. Pure function over *st (no statics),
 * so it can be driven by either a plain while-loop or a GTK timer. */
static int bhop_step(const struct cfg *c, unsigned long base, int strafe,
                     struct bhop_state *st) {
	unsigned long hero = 0;
	if (resolve(c, base, &hero) < 0) return BHOP_RESOLVEFAIL;
	uint32_t alive = ru(hero + c->h_alive);

	if (alive == 0) {
		if (st->jumps > 0) printf("\n[DEAD] jumps=%d\n", st->jumps);
		st->jumps = 0; st->was_air = 0; st->prev_alive = 0;
		return BHOP_DEAD;
	}

	int air = (alive == c->state_air || alive == c->state_fall);
	int held = space_held(st->dpy, st->space_kc);

	if (held) {
		wf(hero + c->h_timing, c->timing_val);
		wu(hero + c->h_perf, 1);

		if (strafe && air) {
			wf(hero + c->h_spd_a, c->boost_accel);
			wf(hero + c->h_spd_b, c->boost_accel);
			wf(hero + c->h_spd_c, c->boost_spd_c);
			wf(hero + c->h_spd_d, c->boost_spd_d);
			wf(hero + c->h_spd_e, c->boost_spd_e);
			wf(hero + c->h_frict, c->boost_frict);

			float py = rf(hero + c->h_pos_y);
			float vy = rf(hero + c->h_vel_y);
			if (py < c->max_boost_alt) {
				wf(hero + c->h_vel_y, vy + c->gravity_cut);
			}

			st->log_tick++;
			if (st->log_tick % 50 == 0) {
				float vx = rf(hero + c->h_vel_x), vz = rf(hero + c->h_vel_z);
				float speed = sqrtf(vx*vx + vz*vz);
				float yaw = rf(hero + c->h_yaw);
				LOG("[%d] state=%d hspd=%.2f py=%.2f vy=%.2f grav=%s yaw=%.2f\n",
				    st->log_tick, alive, speed, py, vy,
				    py < c->max_boost_alt ? "on" : "off", yaw);
			}
		}
	}

	if (st->was_air && alive == c->state_air && st->prev_alive != c->state_air)
		st->jumps++;

	st->was_air = air;
	st->prev_alive = alive;

	if (st->jumps % 3 == 0 || !air) {
		float py = rf(hero + c->h_pos_y);
		float vx = rf(hero + c->h_vel_x), vz = rf(hero + c->h_vel_z);
		float hs = sqrtf(vx*vx + vz*vz);
		float sa = rf(hero + c->h_spd_a), fr = rf(hero + c->h_frict);
		printf("\r[bhop] py=%.3f %s jumps=%d hspd=%.1f accel=%.1f fr=%.4f%s   ",
		       py, state_str(c, alive), st->jumps, hs, sa, fr,
		       strafe && air ? " [BOOST]" : "");
		fflush(stdout);
	}
	return BHOP_OK;
}

static void run_bhop(const struct cfg *c, unsigned long base, int strafe) {
	struct bhop_state st = {0};

	st.dpy = XOpenDisplay(NULL);
	if (!st.dpy) { fprintf(stderr, "no X display\n"); return; }
	st.space_kc = XKeysymToKeycode(st.dpy, XK_space);

	if (strafe) {
		logfile = fopen(LOG_PATH, "w");
		if (logfile) fprintf(logfile, "daggerfall strafe log\n");
	}

	printf("bhop%s — hold space — Ctrl-C to stop\n\n", strafe ? " +strafe" : " (timing)");
	while (running) {
		int r = bhop_step(c, base, strafe, &st);
		/* Exact pre-refactor cadence: 10ms on dead, 2ms otherwise. */
		usleep(r == BHOP_DEAD ? 10000 : POLL_US);
	}
	if (logfile) fclose(logfile);
	printf("\nstopped. jumps=%d\n", st.jumps);
	XCloseDisplay(st.dpy);
}

static void run_teleport(const struct cfg *c, unsigned long base, float tx, float tz) {
	unsigned long hero=0;
	if(resolve(c,base,&hero)<0){fprintf(stderr,"chain fail\n");return;}
	float ox=rf(hero+c->h_pos_x), oz=rf(hero+c->h_pos_z);
	wf(hero+c->h_pos_x, tx); wf(hero+c->h_pos_z, tz);
	printf("teleported (%.2f,%.2f) -> (%.2f,%.2f)\n",ox,oz,tx,tz);
}

static void usage(const char *p){
	fprintf(stderr,"daggerfall — Devil Daggers bhop + air control\n"
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

	/* try exe dir, then cwd, then ~/.config */
	int loaded = 0;
	char exe_path[512];
	ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path)-1);
	if (len > 0) {
		exe_path[len] = 0;
		char *slash = strrchr(exe_path, '/');
		if (slash) { strcpy(slash+1, "daggerfall.conf"); if(load_config(exe_path,&c,mode)==0) loaded=1; }
	}
	if (!loaded) { char *cwd = get_current_dir_name(); if(cwd){char p[512];snprintf(p,512,"%s/daggerfall.conf",cwd);if(load_config(p,&c,mode)==0)loaded=1;free(cwd);} }
	if (!loaded) { char p[256]; snprintf(p,256,"%s/.config/daggerfall.conf",getenv("HOME")?:"/tmp"); if(load_config(p,&c,mode)==0)loaded=1; }
	if (!loaded) { fprintf(stderr,"daggerfall.conf not found. See example.conf.\n"); return 1; }

	FILE*pg=popen("pgrep -x devildaggers","r");
	if(!pg||fscanf(pg,"%d",&pid)!=1){fprintf(stderr,"not running\n");return 1;}pclose(pg);
	unsigned long base=get_base(); if(!base){fprintf(stderr,"no base\n");return 1;}
	printf("pid=%d base=0x%lx\n",pid,base);
	signal(SIGINT,sighandler); signal(SIGTERM,sighandler);
	switch(mode){
		case 0: run_bhop(&c,base,strafe); break;
		case 1: run_diag(&c,base); break;
		case 2: run_teleport(&c,base,tx,tz); break;
		case 3: run_aim(&c,base,strafe); break;
	}
	return 0;
}
