/*
 * wrap_lib.c
 * Copyright (C) 2013 Jeremy C. Andrus <jeremya@cs.columbia.edu>
 *
 * Glue to wrap up a library (including possibly libc) and trace certain
 * functions within that library.
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>

#include "wrap_lib.h"

extern const char *progname; /* comes from real libc */

/* from backtrace.c */
extern void log_backtrace(FILE *logf, const char *sym);

static int local_strcmp(const char *s1, const char *s2)
{
	while (*s1 == *s2++)
		if (*s1++ == 0)
			return (0);
	return (*(unsigned char *)s1 - *(unsigned char *)--s2);
}

struct libc_iface libc __attribute__((visibility("hidden")));

/*
 * Symbol table / information
 *
 */
#undef SAVED
#undef SYM
#define SAVED(addr,name) #name
#define SYM(addr,name)
static const char *wrapped_sym=
#include "real_syms.h"
;
#undef SAVED
#undef SYM

#define SAVED(addr,name)
#define SYM(addr,name) { #name, (unsigned long)(addr) },
struct symbol {
	const char *name;
	unsigned long offset;
} sym_table[] = {
#include "real_syms.h"
	{ NULL, 0 },
};

static Dl_info wrapped_dli;

/*
 * Use our internal offset table to locate the symbol within the
 * given DSO handle.
 *
 */
static void *table_dlsym(void *dso, const char *sym)
{
	struct symbol *symbol;

	if (!wrapped_dli.dli_fbase) {
		void *sym;
		/* get the address of a symbol we know exists in the library */
		sym = dlsym(dso, wrapped_sym);
		if (!sym)
			_BUG(0x1);
		if (dladdr(sym, &wrapped_dli) == 0)
			_BUG(0x2);
	}

	for (symbol = &sym_table[0]; symbol->name; symbol++) {
		if (local_strcmp(symbol->name, sym) == 0)
			break;
	}
	if (!symbol->name)
		return NULL;

	return (void *)((char *)wrapped_dli.dli_fbase + symbol->offset);
}

static FILE *get_log(void)
{
	static pthread_key_t log_key = (pthread_key_t)(-1);
	FILE *logf = NULL;

	if (!libc.dso)
		if (init_libc_iface(&libc, LIBC_PATH) < 0)
			_BUG(0x10);

	if (log_key == (pthread_key_t)(-1))
		libc.pthread_key_create(&log_key, NULL);

	logf = (FILE *)libc.pthread_getspecific(log_key);
	if (!logf) {
		char buf[256];
		libc.snprintf(buf, sizeof(buf), "%s/%d.%d.%s.log",
			      LOGFILE_PATH, libc.getpid(), libc.gettid(), progname);
		/* libc.printf("Creating logfile: %s\n", buf); */
		logf = libc.fopen(buf, "a+");
		/* libc.printf("\topen=%d\n", libc.fno(logf)); */
		if (!logf)
			return NULL;
		libc.fprintf(logf, "STARTED LOG\n");
		libc.pthread_setspecific(log_key, logf);
	}
	return logf;
}

#define libc_log(fmt, ...) \
	do { \
		FILE *f; \
		f = get_log(); \
		if (f) \
			libc.fprintf(f, fmt "\n", ## __VA_ARGS__ ); \
		/* libc.printf("\tLOG:%d: " fmt "\n", libc.fno(f),  ## __VA_ARGS__); */ \
	} while (0)

#define BUG(X) \
	do { \
		FILE *f; \
		f = get_log(); \
		if (f) { \
			libc_log("BUG(0x%x) at %s:%d\n", X, __FILE__, __LINE__); \
			libc.fclose(f); \
		} \
		_BUG(X); \
	} while (0)

/**
 * @wrapped_dlsym Locate a symbol within a library, possibly loading the lib.
 * @param libpath Full path to the library in which symbol should be found
 * @param lib_handle Pointer to memory location to store loaded library handle
 * @param symbol String name of the symbol to be found in libpath
 *
 * This function is called whenever a library must be loaded, or whenever
 * a symbol must be located (within a given library)
 */
void *wrapped_dlsym(const char *libpath, void **lib_handle, const char *symbol)
{
	void *sym;

	/*
	libc_log("lib:%s,handle:%p(%p),sym:%s", libpath, lib_handle,
		 lib_handle ? *lib_handle : NULL, symbol);
	 */
	if (!lib_handle)
		BUG(0x21);

	if (!*lib_handle) {
		*lib_handle = dlopen(libpath, RTLD_NOW | RTLD_LOCAL);
		if (!*lib_handle)
			BUG(0x22);
	}

	sym = table_dlsym(*lib_handle, symbol);
	if (!sym)
		BUG(0x23);
	return sym;
}

/**
 * @wrapped_tracer Default tracing function that stores a backtrace
 * @param symbol The symbol from which wrapped_tracer is being called
 *
 */
void wrapped_tracer(const char *symbol)
{
	static pthread_key_t wrap_key = (pthread_key_t)(-1);
	uint32_t wrapping = 0;

	if (!libc.dso)
		if (init_libc_iface(&libc, LIBC_PATH) < 0)
			BUG(0x30);
	if (wrap_key == (pthread_key_t)(-1)) {
		libc.pthread_key_create(&wrap_key, NULL);
		libc.pthread_setspecific(wrap_key, (const void *)0);
	}

	/* quick check for recursive calls */
	wrapping = (uint32_t)libc.pthread_getspecific(wrap_key);
	if (wrapping)
		return;
	libc.pthread_setspecific(wrap_key, (const void *)1);

	/* libc_log("%s", symbol); */
	log_backtrace(get_log(), symbol);

	libc.pthread_setspecific(wrap_key, (const void *)0);
	return;
}

int init_libc_iface(struct libc_iface *iface, const char *dso_path)
{
	if (!iface->dso) {
		iface->dso = dlopen(dso_path, RTLD_NOW | RTLD_LOCAL);
		if (!iface->dso)
			_BUG(0x40);
	}
#define init_sym(iface,req,sym,alt) \
	do { \
		if ((iface)->sym) \
			break; \
		(iface)->sym = (typeof ((iface)->sym))table_dlsym((iface)->dso, #sym); \
		if ((iface)->sym) \
			break; \
		(iface)->sym = (typeof ((iface)->sym))table_dlsym((iface)->dso, "_" #sym); \
		if ((iface)->sym) \
			break; \
		(iface)->sym = (typeof ((iface)->sym))table_dlsym((iface)->dso, #alt); \
		if ((iface)->sym) \
			break; \
		(iface)->sym = (typeof ((iface)->sym))table_dlsym((iface)->dso, "_" #alt); \
		if ((iface)->sym) \
			break; \
		if (req) \
			_BUG(0x41); \
	} while (0)

	init_sym(iface, 1, fopen,);
	init_sym(iface, 1, fclose,);
	init_sym(iface, 1, fwrite,);
	init_sym(iface, 1, fno, fileno);
	init_sym(iface, 1, getpid,);
	init_sym(iface, 1, gettid, __thread_selfid);
	init_sym(iface, 1, pthread_key_create,);
	init_sym(iface, 1, pthread_getspecific,);
	init_sym(iface, 1, pthread_setspecific,);
	init_sym(iface, 1, snprintf,);
	init_sym(iface, 1, printf,);
	init_sym(iface, 1, fprintf,);
	init_sym(iface, 1, memset,);
	init_sym(iface, 1, malloc,);
	init_sym(iface, 1, free,);

	/* backtrace interface */
	init_sym(iface, 0, backtrace,);
	init_sym(iface, 0, backtrace_symbols,);

	/* unwind interface */
	init_sym(iface, 0, _Unwind_GetIP,);
#ifdef __arm__
	init_sym(iface, 0, _Unwind_VRS_Get,);
#endif
	init_sym(iface, 0, _Unwind_Backtrace,);

	return 0;
}

