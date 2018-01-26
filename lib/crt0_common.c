#include<libtransistor/context.h>
#include<libtransistor/util.h>
#include<libtransistor/svc.h>
#include<libtransistor/ipc/bsd.h>
#include<libtransistor/ipc/sm.h>

#include<sys/socket.h>
#include<assert.h>
#include<stdio.h>
#include<string.h>
#include<unistd.h>
#include<errno.h>

#include<ssp/ssp.h>

int main(int argc, char **argv);

#define make_ip(a,b,c,d)	((a) | ((b) << 8) | ((c) << 16) | ((d) << 24))

struct sockaddr_in stdout_server_addr =
{
	.sin_family = AF_INET,
	.sin_port = htons(2991),
	.sin_addr = {
		.s_addr = make_ip(91, 121, 81, 160)
	}
};


// from util.c
extern size_t log_length;
extern char log_buffer[0x20000];

typedef struct {
	uint32_t magic, dynamic_off, bss_start_off, bss_end_off;
	uint32_t unwind_start_off, unwind_end_off, module_object_off;
} module_header_t;

typedef struct {
	int64_t d_tag;
	union {
		uint64_t d_val;
		void *d_ptr;
	};
} Elf64_Dyn;

typedef struct {
	uint64_t r_offset;
	uint32_t r_reloc_type;
	uint32_t r_symbol;
	uint64_t r_addend;
} Elf64_Rela;

static_assert(sizeof(Elf64_Rela) == 0x18, "Elf64_Rela size should be 0x18");

// defined in crt0.nxo.S, mostly to avoid using the GOT before we relocate its entries
extern module_header_t *_get_module_header();

static bool relocate(uint8_t *aslr_base) {
	module_header_t *mod_header = _get_module_header();
	Elf64_Dyn *dynamic = (Elf64_Dyn*) (((uint8_t*) mod_header) + mod_header->dynamic_off);
	uint64_t rela_offset = 0;
	uint64_t rela_size = 0;
	uint64_t rela_ent = 0;
	uint64_t rela_count = 0;
	bool found_rela = false;
  
	while(dynamic->d_tag > 0) {
		switch(dynamic->d_tag) {
		case 7: // DT_RELA
			if(found_rela) {
				return true;
			}
			rela_offset = dynamic->d_val;
			found_rela = true;
			break;
		case 8: // DT_RELASZ
			rela_size = dynamic->d_val;
			break;
		case 9: // DT_RELAENT
			rela_ent = dynamic->d_val;
			break;
		case 16: // DT_SYMBOLIC
			break;
		case 0x6ffffff9: // DT_RELACOUNT
			rela_count = dynamic->d_val;
			break;
		}
		dynamic++;
	}
  
	if(rela_ent != 0x18) {
		return true;
	}
  
	if(rela_size != rela_count * rela_ent) {
		return true;
	}
  
	Elf64_Rela *rela_base = (Elf64_Rela*) (aslr_base + rela_offset);
	for(uint64_t i = 0; i < rela_count; i++) {
		Elf64_Rela rela = rela_base[i];
    
		switch(rela.r_reloc_type) {
		case 0x403: // R_AARCH64_RELATIVE
			if(rela.r_symbol != 0) {
				return true;
			}
			*(void**)(aslr_base + rela.r_offset) = aslr_base + rela.r_addend;
			break;
		default:
			return true;
		}
	}

	return false;
}

static FILE bsslog_stdout;
static int bsslog_write(struct _reent *reent, void *v, const char *ptr, int len) {
	log_string(ptr, len);
	return len;
}

#define DEFAULT_NOCONTEXT_HEAP_SIZE 0x400000

static bool dont_finalize_bsd = false;

// Set in the linker script.
extern u8 *NORELOC_TDATA_START_;
extern u8 *NORELOC_TDATA_END_;
extern u8 *NORELOC_TBSS_START_;
extern u8 *NORELOC_TBSS_END_;

int _libtransistor_start(libtransistor_context_t *ctx, void *aslr_base) {
	if(relocate(aslr_base)) {
		return -4;
	}

	__guard_setup();

	dbg_printf("aslr base: %p", aslr_base);
	dbg_printf("ctx: %p", ctx);

	char *argv_default[] = {"contextless", NULL};
	char **argv = argv_default;
	int argc = 1;

	libtransistor_context_t empty_context;
	memset(&empty_context, 0, sizeof(empty_context));
  
	if(ctx != NULL) {
		dbg_printf("found context");
		dbg_printf("  magic: 0x%x", ctx->magic);
		dbg_printf("  version: %d", ctx->version);
		dbg_printf("  size: 0x%x", ctx->size);

		if(ctx->magic != LIBTRANSISTOR_CONTEXT_MAGIC) {
			dbg_printf("invalid context magic");
			return -2;
		}
    
		ctx->log_buffer = log_buffer;
		ctx->log_length = &log_length;
		ctx->return_flags = 0;
    
		argv = ctx->argv;
		argc = (int) ctx->argc;

		if(ctx->version != LIBTRANSISTOR_CONTEXT_VERSION) {
			dbg_printf("mismatched context version");
			return -2;
		}
    
		if(ctx->size != sizeof(libtransistor_context_t)) {
			dbg_printf("mismatched context size");
			return -3;
		}

		libtransistor_context = ctx;
	} else {
		dbg_printf("no context");

		libtransistor_context = &empty_context;
		if(svcSetHeapSize(&libtransistor_context->mem_base, DEFAULT_NOCONTEXT_HEAP_SIZE) != RESULT_OK) {
			dbg_printf("failed to set heap size");
			return -5;
		}
		libtransistor_context->mem_size = DEFAULT_NOCONTEXT_HEAP_SIZE;

		do {
			if (sm_init() != RESULT_OK)
				break;
			if (bsd_init() != RESULT_OK)
				break;
			int std_sck = bsd_socket(AF_INET, SOCK_STREAM, 6); // PROTO_TCP
			if(std_sck < 0)
				break;
			// connect to stdout server, optional
			if(bsd_connect(std_sck, (struct sockaddr*) &stdout_server_addr, sizeof(stdout_server_addr)) < 0)
			{
				bsd_close(std_sck);
				std_sck = -1; // invalidate
			}
			if (std_sck >= 0) {
				libtransistor_context->std_socket = std_sck;
				libtransistor_context->has_bsd = true;
			}
		} while(0);
	}

	dbg_printf("init stdio");
	bsslog_stdout._write = bsslog_write;
	bsslog_stdout._flags = __SWR | __SNBF;
	bsslog_stdout._bf._base = (void*) 1;

	if(libtransistor_context->has_bsd && libtransistor_context->std_socket > 0) {
		dbg_printf("using socklog stdio");
		bsd_init(); // borrow bsd object from loader
		int fd = socket_from_bsd(libtransistor_context->std_socket);
		if (fd < 0) {
			dbg_printf("Error creating socket: %d", errno);
		} else {
			if (dup2(fd, STDIN_FILENO) < 0)
				dbg_printf("Error setting up stdin: %d", errno);
			if (dup2(fd, STDOUT_FILENO) < 0)
				dbg_printf("Error setting up stdout: %d", errno);
			if (dup2(fd, STDERR_FILENO) < 0)
				dbg_printf("Error setting up stderr: %d", errno);
		}
	} else {
		// TODO: Create a fake FD for bsslog
		dbg_printf("using bsslog stdout");
		printf("_"); // init stdout
		getchar(); // init stdin
		stdout = &bsslog_stdout;
		stderr = &bsslog_stdout;
	}
	dbg_printf("set up stdout");
	
	int ret = main(argc, argv);

	if(libtransistor_context->has_bsd && libtransistor_context->std_socket > 0 && !dont_finalize_bsd) {
		bsd_finalize();
	}

	return ret;
}

void libtransistor_dont_finalize_bsd() {
	dont_finalize_bsd = true;
}
