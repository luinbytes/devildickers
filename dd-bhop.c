/*
 * dd-bhop — bhop + air control for Devil Daggers
 *
 * Usage:
 *   dd-bhop              — bhop only (hold space)
 *   dd-bhop --strafe     — bhop + air control boost
 *   dd-bhop --diag       — live diagnostic
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
#include <X11/keysym.h>

#define POLL_US  2000
#define LOG_PATH "/tmp/dd-strafe.log"

struct cfg {
	unsigned long off_globals, off_arena, off_hero;
	unsigned long h_pos_x, h_pos_y, h_pos_z;
	unsigned long h_vel_x, h_vel_y, h_vel_z;
	unsigned long h_yaw, h_alive;
	unsigned long h_spd_a, h_spd_b, h_spd_c, h_spd_d, h_spd_e;
	unsigned long h_frict, h_timing, h_perf;
	uint32_t state_gnd, state_air, state_fall;
	float timing_val;
	float boost_accel;
	float boost_spd_c, boost_spd_d, boost_spd_e;
	float boost_frict;
	float gravity_cut, max_boost_alt;
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
	    "  %s --teleport X Z\n",p,p,p,p);
}

int main(int argc, char **argv){
	int mode=0, strafe=0; float tx=0,tz=0;
	for(int i=1;i<argc;i++){
		if(!strcmp(argv[i],"--diag")) mode=1;
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
	switch(mode){case 0:run_bhop(&c,base,strafe);break;case 1:run_diag(&c,base);break;case 2:run_teleport(&c,base,tx,tz);break;}
	return 0;
}
