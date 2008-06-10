/* outelf.c	output routines for the Netwide Assembler to produce
 *		ELF32 (i386 of course) object file format
 *
 * The Netwide Assembler is copyright (C) 1996 Simon Tatham and
 * Julian Hall. All rights reserved. The software is
 * redistributable under the license given in the file "LICENSE"
 * distributed in the NASM archive.
 */

#include "compiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>

#include "nasm.h"
#include "nasmlib.h"
#include "stdscan.h"
#include "outform.h"
#include "saa.h"

#ifdef OF_ELF32

/*
 * Relocation types.
 */
enum reloc_type {
    R_386_32 = 1,               /* ordinary absolute relocation */
    R_386_PC32 = 2,             /* PC-relative relocation */
    R_386_GOT32 = 3,            /* an offset into GOT */
    R_386_PLT32 = 4,            /* a PC-relative offset into PLT */
    R_386_COPY = 5,             /* ??? */
    R_386_GLOB_DAT = 6,         /* ??? */
    R_386_JUMP_SLOT = 7,        /* ??? */
    R_386_RELATIVE = 8,         /* ??? */
    R_386_GOTOFF = 9,           /* an offset from GOT base */
    R_386_GOTPC = 10,           /* a PC-relative offset _to_ GOT */
    /* These are GNU extensions, but useful */
    R_386_16 = 20,              /* A 16-bit absolute relocation */
    R_386_PC16 = 21,            /* A 16-bit PC-relative relocation */
    R_386_8 = 22,               /* An 8-bit absolute relocation */
    R_386_PC8 = 23              /* An 8-bit PC-relative relocation */
};

struct Reloc {
    struct Reloc *next;
    int32_t address;               /* relative to _start_ of section */
    int32_t symbol;                /*  symbol index */
    int type;                   /* type of relocation */
};

struct Symbol {
    int32_t strpos;                /* string table position of name */
    int32_t section;               /* section ID of the symbol */
    int type;                   /* symbol type */
    int other;                     /* symbol visibility */
    int32_t value;                 /* address, or COMMON variable align */
    int32_t size;                  /* size of symbol */
    int32_t globnum;               /* symbol table offset if global */
    struct Symbol *next;        /* list of globals in each section */
    struct Symbol *nextfwd;     /* list of unresolved-size symbols */
    char *name;                 /* used temporarily if in above list */
};

#define SHT_PROGBITS 1
#define SHT_NOBITS 8

#define SHF_WRITE 1
#define SHF_ALLOC 2
#define SHF_EXECINSTR 4

struct Section {
    struct SAA *data;
    uint32_t len, size, nrelocs;
    int32_t index;
    int type;                   /* SHT_PROGBITS or SHT_NOBITS */
    int align;                  /* alignment: power of two */
    uint32_t flags;        /* section flags */
    char *name;
    struct SAA *rel;
    int32_t rellen;
    struct Reloc *head, **tail;
    struct Symbol *gsyms;       /* global symbols in section */
};

#define SECT_DELTA 32
static struct Section **sects;
static int nsects, sectlen;

#define SHSTR_DELTA 256
static char *shstrtab;
static int shstrtablen, shstrtabsize;

static struct SAA *syms;
static uint32_t nlocals, nglobs;

static int32_t def_seg;

static struct RAA *bsym;

static struct SAA *strs;
static uint32_t strslen;

static FILE *elffp;
static efunc error;
static evalfunc evaluate;

static struct Symbol *fwds;

static char elf_module[FILENAME_MAX];

static uint8_t elf_osabi = 0;	/* Default OSABI = 0 (System V or Linux) */
static uint8_t elf_abiver = 0;	/* Current ABI version */

extern struct ofmt of_elf32;
extern struct ofmt of_elf;

#define SHN_ABS 0xFFF1
#define SHN_COMMON 0xFFF2
#define SHN_UNDEF 0

#define SYM_GLOBAL 0x10

#define SHT_RELA	  4		/* Relocation entries with addends */

#define STT_NOTYPE	0		/* Symbol type is unspecified */
#define STT_OBJECT	1		/* Symbol is a data object */
#define STT_FUNC	2		/* Symbol is a code object */
#define STT_SECTION	3		/* Symbol associated with a section */
#define STT_FILE	4		/* Symbol's name is file name */
#define STT_COMMON	5		/* Symbol is a common data object */
#define STT_TLS		6		/* Symbol is thread-local data object*/
#define	STT_NUM		7		/* Number of defined types.  */

#define STV_DEFAULT 0
#define STV_INTERNAL 1
#define STV_HIDDEN 2
#define STV_PROTECTED 3

#define GLOBAL_TEMP_BASE 1048576     /* bigger than any reasonable sym id */

#define SEG_ALIGN 16            /* alignment of sections in file */
#define SEG_ALIGN_1 (SEG_ALIGN-1)

/* Definitions in lieu of dwarf.h */
#define    DW_TAG_compile_unit   0x11
#define    DW_TAG_subprogram   0x2e
#define    DW_AT_name   0x03
#define    DW_AT_stmt_list   0x10
#define    DW_AT_low_pc   0x11
#define    DW_AT_high_pc   0x12
#define    DW_AT_language  0x13
#define    DW_AT_producer   0x25
#define    DW_AT_frame_base   0x40
#define    DW_FORM_addr   0x01
#define    DW_FORM_data2   0x05
#define    DW_FORM_data4   0x06
#define    DW_FORM_string   0x08
#define    DW_LNS_extended_op  0
#define    DW_LNS_advance_pc   2
#define    DW_LNS_advance_line   3
#define    DW_LNS_set_file   4
#define    DW_LNE_end_sequence   1
#define    DW_LNE_set_address   2
#define    DW_LNE_define_file   3
#define    DW_LANG_Mips_Assembler  0x8001

#define SOC(ln,aa) ln - line_base + (line_range * aa) + opcode_base

static const char align_str[SEG_ALIGN] = "";    /* ANSI will pad this with 0s */

static struct ELF_SECTDATA {
    void *data;
    int32_t len;
    bool is_saa;
} *elf_sects;
static int elf_nsect, nsections;
static int32_t elf_foffs;

static void elf_write(void);
static void elf_sect_write(struct Section *, const uint8_t *,
                           uint32_t);
static void elf_section_header(int, int, int, void *, bool, int32_t, int, int,
                               int, int);
static void elf_write_sections(void);
static struct SAA *elf_build_symtab(int32_t *, int32_t *);
static struct SAA *elf_build_reltab(int32_t *, struct Reloc *);
static void add_sectname(char *, char *);

/* this stuff is needed for the stabs debugging format */
#define N_SO 0x64               /* ID for main source file */
#define N_SOL 0x84              /* ID for sub-source file */
#define N_BINCL 0x82
#define N_EINCL 0xA2
#define N_SLINE 0x44
#define TY_STABSSYMLIN 0x40     /* ouch */

struct stabentry {
    uint32_t n_strx;
    uint8_t n_type;
    uint8_t n_other;
    uint16_t n_desc;
    uint32_t n_value;
};

struct erel {
    int offset, info;
};

struct symlininfo {
    int offset;
    int section;                /* section index */
    char *name;                 /* shallow-copied pointer of section name */
};

struct linelist {
    struct symlininfo info;
    int line;
    char *filename;
    struct linelist *next;
    struct linelist *last;
};

struct sectlist {
    struct SAA *psaa;
    int section;
    int line;
    int offset;
    int file;
    struct sectlist *next;
    struct sectlist *last;
};

/* common debug variables */
static int currentline = 1;
static int debug_immcall = 0;

/* stabs debug variables */
static struct linelist *stabslines = 0;
static int numlinestabs = 0;
static char *stabs_filename = 0;
static int symtabsection;
static uint8_t *stabbuf = 0, *stabstrbuf = 0, *stabrelbuf = 0;
static int stablen, stabstrlen, stabrellen;

/* dwarf debug variables */
static struct linelist *dwarf_flist = 0, *dwarf_clist = 0, *dwarf_elist = 0;
static struct sectlist *dwarf_fsect = 0, *dwarf_csect = 0, *dwarf_esect = 0;
static int dwarf_numfiles = 0, dwarf_nsections;
static uint8_t *arangesbuf = 0, *arangesrelbuf = 0, *pubnamesbuf = 0, *infobuf = 0,  *inforelbuf = 0,
               *abbrevbuf = 0, *linebuf = 0, *linerelbuf = 0, *framebuf = 0, *locbuf = 0;
static int8_t line_base = -5, line_range = 14, opcode_base = 13;
static int arangeslen, arangesrellen, pubnameslen, infolen, inforellen,
           abbrevlen, linelen, linerellen, framelen, loclen;
static int32_t dwarf_infosym, dwarf_abbrevsym, dwarf_linesym;

static struct dfmt df_dwarf;
static struct dfmt df_stabs;
static struct Symbol *lastsym;

/* common debugging routines */
void debug32_typevalue(int32_t);
void debug32_init(struct ofmt *, void *, FILE *, efunc);
void debug32_deflabel(char *, int32_t, int64_t, int, char *);
void debug32_directive(const char *, const char *);

/* stabs debugging routines */
void stabs32_linenum(const char *filename, int32_t linenumber, int32_t);
void stabs32_output(int, void *);
void stabs32_generate(void);
void stabs32_cleanup(void);

/* dwarf debugging routines */
void dwarf32_linenum(const char *filename, int32_t linenumber, int32_t);
void dwarf32_output(int, void *);
void dwarf32_generate(void);
void dwarf32_cleanup(void);
void dwarf32_findfile(const char *);
void dwarf32_findsect(const int);
void saa_wleb128u(struct SAA *, int);
void saa_wleb128s(struct SAA *, int);

/*
 * Special section numbers which are used to define ELF special
 * symbols, which can be used with WRT to provide PIC relocation
 * types.
 */
static int32_t elf_gotpc_sect, elf_gotoff_sect;
static int32_t elf_got_sect, elf_plt_sect;
static int32_t elf_sym_sect;

static void elf_init(FILE * fp, efunc errfunc, ldfunc ldef, evalfunc eval)
{
    if (of_elf.current_dfmt != &null_debug_form)
        of_elf32.current_dfmt = of_elf.current_dfmt;
    elffp = fp;
    error = errfunc;
    evaluate = eval;
    (void)ldef;                 /* placate optimisers */
    sects = NULL;
    nsects = sectlen = 0;
    syms = saa_init((int32_t)sizeof(struct Symbol));
    nlocals = nglobs = 0;
    bsym = raa_init();
    strs = saa_init(1L);
    saa_wbytes(strs, "\0", 1L);
    saa_wbytes(strs, elf_module, strlen(elf_module)+1);
    strslen = 2 + strlen(elf_module);
    shstrtab = NULL;
    shstrtablen = shstrtabsize = 0;;
    add_sectname("", "");

    fwds = NULL;

    elf_gotpc_sect = seg_alloc();
    ldef("..gotpc", elf_gotpc_sect + 1, 0L, NULL, false, false, &of_elf32,
         error);
    elf_gotoff_sect = seg_alloc();
    ldef("..gotoff", elf_gotoff_sect + 1, 0L, NULL, false, false, &of_elf32,
         error);
    elf_got_sect = seg_alloc();
    ldef("..got", elf_got_sect + 1, 0L, NULL, false, false, &of_elf32,
         error);
    elf_plt_sect = seg_alloc();
    ldef("..plt", elf_plt_sect + 1, 0L, NULL, false, false, &of_elf32,
         error);
    elf_sym_sect = seg_alloc();
    ldef("..sym", elf_sym_sect + 1, 0L, NULL, false, false, &of_elf32,
         error);

    def_seg = seg_alloc();
}

static void elf_cleanup(int debuginfo)
{
    struct Reloc *r;
    int i;

    (void)debuginfo;

    elf_write();
    fclose(elffp);
    for (i = 0; i < nsects; i++) {
        if (sects[i]->type != SHT_NOBITS)
            saa_free(sects[i]->data);
        if (sects[i]->head)
            saa_free(sects[i]->rel);
        while (sects[i]->head) {
            r = sects[i]->head;
            sects[i]->head = sects[i]->head->next;
            nasm_free(r);
        }
    }
    nasm_free(sects);
    saa_free(syms);
    raa_free(bsym);
    saa_free(strs);
    if (of_elf32.current_dfmt) {
        of_elf32.current_dfmt->cleanup();
    }
}

static void add_sectname(char *firsthalf, char *secondhalf)
{
    int len = strlen(firsthalf) + strlen(secondhalf);
    while (shstrtablen + len + 1 > shstrtabsize)
        shstrtab = nasm_realloc(shstrtab, (shstrtabsize += SHSTR_DELTA));
    strcpy(shstrtab + shstrtablen, firsthalf);
    strcat(shstrtab + shstrtablen, secondhalf);
    shstrtablen += len + 1;
}

static int elf_make_section(char *name, int type, int flags, int align)
{
    struct Section *s;

    s = nasm_malloc(sizeof(*s));

    if (type != SHT_NOBITS)
        s->data = saa_init(1L);
    s->head = NULL;
    s->tail = &s->head;
    s->len = s->size = 0;
    s->nrelocs = 0;
    if (!strcmp(name, ".text"))
        s->index = def_seg;
    else
        s->index = seg_alloc();
    add_sectname("", name);
    s->name = nasm_malloc(1 + strlen(name));
    strcpy(s->name, name);
    s->type = type;
    s->flags = flags;
    s->align = align;
    s->gsyms = NULL;

    if (nsects >= sectlen)
        sects =
            nasm_realloc(sects, (sectlen += SECT_DELTA) * sizeof(*sects));
    sects[nsects++] = s;

    return nsects - 1;
}

static int32_t elf_section_names(char *name, int pass, int *bits)
{
    char *p;
    unsigned flags_and, flags_or;
    int type, align, i;

    /*
     * Default is 32 bits.
     */
    if (!name) {
        *bits = 32;
        return def_seg;
    }

    p = name;
    while (*p && !isspace(*p))
        p++;
    if (*p)
        *p++ = '\0';
    flags_and = flags_or = type = align = 0;

    while (*p && isspace(*p))
        p++;
    while (*p) {
        char *q = p;
        while (*p && !isspace(*p))
            p++;
        if (*p)
            *p++ = '\0';
        while (*p && isspace(*p))
            p++;

        if (!nasm_strnicmp(q, "align=", 6)) {
            align = atoi(q + 6);
            if (align == 0)
                align = 1;
            if ((align - 1) & align) {  /* means it's not a power of two */
                error(ERR_NONFATAL, "section alignment %d is not"
                      " a power of two", align);
                align = 1;
            }
        } else if (!nasm_stricmp(q, "alloc")) {
            flags_and |= SHF_ALLOC;
            flags_or |= SHF_ALLOC;
        } else if (!nasm_stricmp(q, "noalloc")) {
            flags_and |= SHF_ALLOC;
            flags_or &= ~SHF_ALLOC;
        } else if (!nasm_stricmp(q, "exec")) {
            flags_and |= SHF_EXECINSTR;
            flags_or |= SHF_EXECINSTR;
        } else if (!nasm_stricmp(q, "noexec")) {
            flags_and |= SHF_EXECINSTR;
            flags_or &= ~SHF_EXECINSTR;
        } else if (!nasm_stricmp(q, "write")) {
            flags_and |= SHF_WRITE;
            flags_or |= SHF_WRITE;
        } else if (!nasm_stricmp(q, "nowrite")) {
            flags_and |= SHF_WRITE;
            flags_or &= ~SHF_WRITE;
        } else if (!nasm_stricmp(q, "progbits")) {
            type = SHT_PROGBITS;
        } else if (!nasm_stricmp(q, "nobits")) {
            type = SHT_NOBITS;
        }
    }

    if (!strcmp(name, ".comment") ||
        !strcmp(name, ".shstrtab") ||
        !strcmp(name, ".symtab") || !strcmp(name, ".strtab")) {
        error(ERR_NONFATAL, "attempt to redefine reserved section"
              "name `%s'", name);
        return NO_SEG;
    }

    for (i = 0; i < nsects; i++)
        if (!strcmp(name, sects[i]->name))
            break;
    if (i == nsects) {
        if (!strcmp(name, ".text"))
            i = elf_make_section(name, SHT_PROGBITS,
                                 SHF_ALLOC | SHF_EXECINSTR, 16);
        else if (!strcmp(name, ".rodata"))
            i = elf_make_section(name, SHT_PROGBITS, SHF_ALLOC, 4);
        else if (!strcmp(name, ".data"))
            i = elf_make_section(name, SHT_PROGBITS,
                                 SHF_ALLOC | SHF_WRITE, 4);
        else if (!strcmp(name, ".bss"))
            i = elf_make_section(name, SHT_NOBITS,
                                 SHF_ALLOC | SHF_WRITE, 4);
        else
            i = elf_make_section(name, SHT_PROGBITS, SHF_ALLOC, 1);
        if (type)
            sects[i]->type = type;
        if (align)
            sects[i]->align = align;
        sects[i]->flags &= ~flags_and;
        sects[i]->flags |= flags_or;
    } else if (pass == 1) {
          if ((type && sects[i]->type != type)
             || (align && sects[i]->align != align)
             || (flags_and && ((sects[i]->flags & flags_and) != flags_or)))
            error(ERR_WARNING, "section attributes ignored on"
                  " redeclaration of section `%s'", name);
    }

    return sects[i]->index;
}

static void elf_deflabel(char *name, int32_t segment, int64_t offset,
                         int is_global, char *special)
{
    int pos = strslen;
    struct Symbol *sym;
    bool special_used = false;

#if defined(DEBUG) && DEBUG>2
    fprintf(stderr,
            " elf_deflabel: %s, seg=%ld, off=%ld, is_global=%d, %s\n",
            name, segment, offset, is_global, special);
#endif
    if (name[0] == '.' && name[1] == '.' && name[2] != '@') {
        /*
         * This is a NASM special symbol. We never allow it into
         * the ELF symbol table, even if it's a valid one. If it
         * _isn't_ a valid one, we should barf immediately.
         */
        if (strcmp(name, "..gotpc") && strcmp(name, "..gotoff") &&
            strcmp(name, "..got") && strcmp(name, "..plt") &&
            strcmp(name, "..sym"))
            error(ERR_NONFATAL, "unrecognised special symbol `%s'", name);
        return;
    }

    if (is_global == 3) {
        struct Symbol **s;
        /*
         * Fix up a forward-reference symbol size from the first
         * pass.
         */
        for (s = &fwds; *s; s = &(*s)->nextfwd)
            if (!strcmp((*s)->name, name)) {
                struct tokenval tokval;
                expr *e;
                char *p = special;

                while (*p && !isspace(*p))
                    p++;
                while (*p && isspace(*p))
                    p++;
                stdscan_reset();
                stdscan_bufptr = p;
                tokval.t_type = TOKEN_INVALID;
                e = evaluate(stdscan, NULL, &tokval, NULL, 1, error, NULL);
                if (e) {
                    if (!is_simple(e))
                        error(ERR_NONFATAL, "cannot use relocatable"
                              " expression as symbol size");
                    else
                        (*s)->size = reloc_value(e);
                }

                /*
                 * Remove it from the list of unresolved sizes.
                 */
                nasm_free((*s)->name);
                *s = (*s)->nextfwd;
                return;
            }
        return;                 /* it wasn't an important one */
    }

    saa_wbytes(strs, name, (int32_t)(1 + strlen(name)));
    strslen += 1 + strlen(name);

    lastsym = sym = saa_wstruct(syms);

    sym->strpos = pos;
    sym->type = is_global ? SYM_GLOBAL : 0;
    sym->other = STV_DEFAULT;
    sym->size = 0;
    if (segment == NO_SEG)
        sym->section = SHN_ABS;
    else {
        int i;
        sym->section = SHN_UNDEF;
        if (nsects == 0 && segment == def_seg) {
            int tempint;
            if (segment != elf_section_names(".text", 2, &tempint))
                error(ERR_PANIC,
                      "strange segment conditions in ELF driver");
            sym->section = nsects;
        } else {
            for (i = 0; i < nsects; i++)
                if (segment == sects[i]->index) {
                    sym->section = i + 1;
                    break;
                }
        }
    }

    if (is_global == 2) {
        sym->size = offset;
        sym->value = 0;
        sym->section = SHN_COMMON;
        /*
         * We have a common variable. Check the special text to see
         * if it's a valid number and power of two; if so, store it
         * as the alignment for the common variable.
         */
        if (special) {
            bool err;
            sym->value = readnum(special, &err);
            if (err)
                error(ERR_NONFATAL, "alignment constraint `%s' is not a"
                      " valid number", special);
            else if ((sym->value | (sym->value - 1)) != 2 * sym->value - 1)
                error(ERR_NONFATAL, "alignment constraint `%s' is not a"
                      " power of two", special);
        }
        special_used = true;
    } else
        sym->value = (sym->section == SHN_UNDEF ? 0 : offset);

    if (sym->type == SYM_GLOBAL) {
        /*
         * If sym->section == SHN_ABS, then the first line of the
         * else section would cause a core dump, because its a reference
         * beyond the end of the section array.
         * This behaviour is exhibited by this code:
         *     GLOBAL crash_nasm
         *     crash_nasm equ 0
         * To avoid such a crash, such requests are silently discarded.
         * This may not be the best solution.
         */
        if (sym->section == SHN_UNDEF || sym->section == SHN_COMMON) {
            bsym = raa_write(bsym, segment, nglobs);
        } else if (sym->section != SHN_ABS) {
            /*
             * This is a global symbol; so we must add it to the linked
             * list of global symbols in its section. We'll push it on
             * the beginning of the list, because it doesn't matter
             * much which end we put it on and it's easier like this.
             *
             * In addition, we check the special text for symbol
             * type and size information.
             */
            sym->next = sects[sym->section - 1]->gsyms;
            sects[sym->section - 1]->gsyms = sym;

            if (special) {
                int n = strcspn(special, " \t");

                if (!nasm_strnicmp(special, "function", n))
                    sym->type |= STT_FUNC;
                else if (!nasm_strnicmp(special, "data", n) ||
                         !nasm_strnicmp(special, "object", n))
                    sym->type |= STT_OBJECT;
                else if (!nasm_strnicmp(special, "notype", n))
                    sym->type |= STT_NOTYPE;
                else
                    error(ERR_NONFATAL, "unrecognised symbol type `%.*s'",
                          n, special);
                special += n;

                while (isspace(*special))
                    ++special;
                if (*special) {
                    n = strcspn(special, " \t");
                    if (!nasm_strnicmp(special, "default", n))
                        sym->other = STV_DEFAULT;
                    else if (!nasm_strnicmp(special, "internal", n))
                        sym->other = STV_INTERNAL;
                    else if (!nasm_strnicmp(special, "hidden", n))
                        sym->other = STV_HIDDEN;
                    else if (!nasm_strnicmp(special, "protected", n))
                        sym->other = STV_PROTECTED;
                    else
                        n = 0;
                    special += n;
                }

                if (*special) {
                    struct tokenval tokval;
                    expr *e;
                    int fwd = 0;
                    char *saveme = stdscan_bufptr;      /* bugfix? fbk 8/10/00 */

                    while (special[n] && isspace(special[n]))
                        n++;
                    /*
                     * We have a size expression; attempt to
                     * evaluate it.
                     */
                    stdscan_reset();
                    stdscan_bufptr = special + n;
                    tokval.t_type = TOKEN_INVALID;
                    e = evaluate(stdscan, NULL, &tokval, &fwd, 0, error,
                                 NULL);
                    if (fwd) {
                        sym->nextfwd = fwds;
                        fwds = sym;
                        sym->name = nasm_strdup(name);
                    } else if (e) {
                        if (!is_simple(e))
                            error(ERR_NONFATAL, "cannot use relocatable"
                                  " expression as symbol size");
                        else
                            sym->size = reloc_value(e);
                    }
                    stdscan_bufptr = saveme;    /* bugfix? fbk 8/10/00 */
                }
                special_used = true;
            }
        }
        sym->globnum = nglobs;
        nglobs++;
    } else
        nlocals++;

    if (special && !special_used)
        error(ERR_NONFATAL, "no special symbol features supported here");
}

static void elf_add_reloc(struct Section *sect, int32_t segment, int type)
{
    struct Reloc *r;

    r = *sect->tail = nasm_malloc(sizeof(struct Reloc));
    sect->tail = &r->next;
    r->next = NULL;

    r->address = sect->len;
    if (segment == NO_SEG)
        r->symbol = 0;
    else {
        int i;
        r->symbol = 0;
        for (i = 0; i < nsects; i++)
            if (segment == sects[i]->index)
                r->symbol = i + 2;
        if (!r->symbol)
            r->symbol = GLOBAL_TEMP_BASE + raa_read(bsym, segment);
    }
    r->type = type;

    sect->nrelocs++;
}

/*
 * This routine deals with ..got and ..sym relocations: the more
 * complicated kinds. In shared-library writing, some relocations
 * with respect to global symbols must refer to the precise symbol
 * rather than referring to an offset from the base of the section
 * _containing_ the symbol. Such relocations call to this routine,
 * which searches the symbol list for the symbol in question.
 *
 * R_386_GOT32 references require the _exact_ symbol address to be
 * used; R_386_32 references can be at an offset from the symbol.
 * The boolean argument `exact' tells us this.
 *
 * Return value is the adjusted value of `addr', having become an
 * offset from the symbol rather than the section. Should always be
 * zero when returning from an exact call.
 *
 * Limitation: if you define two symbols at the same place,
 * confusion will occur.
 *
 * Inefficiency: we search, currently, using a linked list which
 * isn't even necessarily sorted.
 */
static int32_t elf_add_gsym_reloc(struct Section *sect,
                               int32_t segment, int32_t offset,
                               int type, bool exact)
{
    struct Reloc *r;
    struct Section *s;
    struct Symbol *sym, *sm;
    int i;

    /*
     * First look up the segment/offset pair and find a global
     * symbol corresponding to it. If it's not one of our segments,
     * then it must be an external symbol, in which case we're fine
     * doing a normal elf_add_reloc after first sanity-checking
     * that the offset from the symbol is zero.
     */
    s = NULL;
    for (i = 0; i < nsects; i++)
        if (segment == sects[i]->index) {
            s = sects[i];
            break;
        }
    if (!s) {
        if (exact && offset != 0)
            error(ERR_NONFATAL, "unable to find a suitable global symbol"
                  " for this reference");
        else
            elf_add_reloc(sect, segment, type);
        return offset;
    }

    if (exact) {
        /*
         * Find a symbol pointing _exactly_ at this one.
         */
        for (sym = s->gsyms; sym; sym = sym->next)
            if (sym->value == offset)
                break;
    } else {
        /*
         * Find the nearest symbol below this one.
         */
        sym = NULL;
        for (sm = s->gsyms; sm; sm = sm->next)
            if (sm->value <= offset && (!sym || sm->value > sym->value))
                sym = sm;
    }
    if (!sym && exact) {
        error(ERR_NONFATAL, "unable to find a suitable global symbol"
              " for this reference");
        return 0;
    }

    r = *sect->tail = nasm_malloc(sizeof(struct Reloc));
    sect->tail = &r->next;
    r->next = NULL;

    r->address = sect->len;
    r->symbol = GLOBAL_TEMP_BASE + sym->globnum;
    r->type = type;

    sect->nrelocs++;

    return offset - sym->value;
}

static void elf_out(int32_t segto, const void *data,
		    enum out_type type, uint64_t size,
                    int32_t segment, int32_t wrt)
{
    struct Section *s;
    int32_t addr;
    uint8_t mydata[4], *p;
    int i;
    static struct symlininfo sinfo;

    /*
     * handle absolute-assembly (structure definitions)
     */
    if (segto == NO_SEG) {
        if (type != OUT_RESERVE)
            error(ERR_NONFATAL, "attempt to assemble code in [ABSOLUTE]"
                  " space");
        return;
    }

    s = NULL;
    for (i = 0; i < nsects; i++)
        if (segto == sects[i]->index) {
            s = sects[i];
            break;
        }
    if (!s) {
        int tempint;            /* ignored */
        if (segto != elf_section_names(".text", 2, &tempint))
            error(ERR_PANIC, "strange segment conditions in ELF driver");
        else {
            s = sects[nsects - 1];
            i = nsects - 1;
        }
    }

    /* again some stabs debugging stuff */
    if (of_elf32.current_dfmt) {
        sinfo.offset = s->len;
        sinfo.section = i;
        sinfo.name = s->name;
        of_elf32.current_dfmt->debug_output(TY_STABSSYMLIN, &sinfo);
    }
    /* end of debugging stuff */

    if (s->type == SHT_NOBITS && type != OUT_RESERVE) {
        error(ERR_WARNING, "attempt to initialize memory in"
              " BSS section `%s': ignored", s->name);
        if (type == OUT_REL2ADR)
            size = 2;
        else if (type == OUT_REL4ADR)
            size = 4;
        s->len += size;
        return;
    }

    if (type == OUT_RESERVE) {
        if (s->type == SHT_PROGBITS) {
            error(ERR_WARNING, "uninitialized space declared in"
                  " non-BSS section `%s': zeroing", s->name);
            elf_sect_write(s, NULL, size);
        } else
            s->len += size;
    } else if (type == OUT_RAWDATA) {
        if (segment != NO_SEG)
            error(ERR_PANIC, "OUT_RAWDATA with other than NO_SEG");
        elf_sect_write(s, data, size);
    } else if (type == OUT_ADDRESS) {
        bool gnu16 = false;
        addr = *(int64_t *)data;
        if (segment != NO_SEG) {
            if (segment % 2) {
                error(ERR_NONFATAL, "ELF format does not support"
                      " segment base references");
            } else {
                if (wrt == NO_SEG) {
                    if (size == 2) {
                        gnu16 = true;
                        elf_add_reloc(s, segment, R_386_16);
                    } else {
                        elf_add_reloc(s, segment, R_386_32);
                    }
                } else if (wrt == elf_gotpc_sect + 1) {
                    /*
                     * The user will supply GOT relative to $$. ELF
                     * will let us have GOT relative to $. So we
                     * need to fix up the data item by $-$$.
                     */
                    addr += s->len;
                    elf_add_reloc(s, segment, R_386_GOTPC);
                } else if (wrt == elf_gotoff_sect + 1) {
                    elf_add_reloc(s, segment, R_386_GOTOFF);
                } else if (wrt == elf_got_sect + 1) {
                    addr = elf_add_gsym_reloc(s, segment, addr,
                                              R_386_GOT32, true);
                } else if (wrt == elf_sym_sect + 1) {
                    if (size == 2) {
                        gnu16 = true;
                        addr = elf_add_gsym_reloc(s, segment, addr,
                                                  R_386_16, false);
                    } else {
                        addr = elf_add_gsym_reloc(s, segment, addr,
                                                  R_386_32, false);
                    }
                } else if (wrt == elf_plt_sect + 1) {
                    error(ERR_NONFATAL, "ELF format cannot produce non-PC-"
                          "relative PLT references");
                } else {
                    error(ERR_NONFATAL, "ELF format does not support this"
                          " use of WRT");
                    wrt = NO_SEG;       /* we can at least _try_ to continue */
                }
            }
        }
        p = mydata;
        if (gnu16) {
            error(ERR_WARNING | ERR_WARN_GNUELF,
                  "16-bit relocations in ELF is a GNU extension");
            WRITESHORT(p, addr);
        } else {
            if (size != 4 && segment != NO_SEG) {
                error(ERR_NONFATAL,
                      "Unsupported non-32-bit ELF relocation");
            }
            WRITELONG(p, addr);
        }
        elf_sect_write(s, mydata, size);
    } else if (type == OUT_REL2ADR) {
        if (segment == segto)
            error(ERR_PANIC, "intra-segment OUT_REL2ADR");
        if (segment != NO_SEG && segment % 2) {
            error(ERR_NONFATAL, "ELF format does not support"
                  " segment base references");
        } else {
            if (wrt == NO_SEG) {
                error(ERR_WARNING | ERR_WARN_GNUELF,
                      "16-bit relocations in ELF is a GNU extension");
                elf_add_reloc(s, segment, R_386_PC16);
            } else {
                error(ERR_NONFATAL,
                      "Unsupported non-32-bit ELF relocation");
            }
        }
        p = mydata;
        WRITESHORT(p, *(int64_t *)data - size);
        elf_sect_write(s, mydata, 2L);
    } else if (type == OUT_REL4ADR) {
        if (segment == segto)
            error(ERR_PANIC, "intra-segment OUT_REL4ADR");
        if (segment != NO_SEG && segment % 2) {
            error(ERR_NONFATAL, "ELF format does not support"
                  " segment base references");
        } else {
            if (wrt == NO_SEG) {
                elf_add_reloc(s, segment, R_386_PC32);
            } else if (wrt == elf_plt_sect + 1) {
                elf_add_reloc(s, segment, R_386_PLT32);
            } else if (wrt == elf_gotpc_sect + 1 ||
                       wrt == elf_gotoff_sect + 1 ||
                       wrt == elf_got_sect + 1) {
                error(ERR_NONFATAL, "ELF format cannot produce PC-"
                      "relative GOT references");
            } else {
                error(ERR_NONFATAL, "ELF format does not support this"
                      " use of WRT");
                wrt = NO_SEG;   /* we can at least _try_ to continue */
            }
        }
        p = mydata;
        WRITELONG(p, *(int64_t *)data - size);
        elf_sect_write(s, mydata, 4L);
    }
}

static void elf_write(void)
{
    int align;
    int scount;
    char *p;
    int commlen;
    char comment[64];
    int i;

    struct SAA *symtab;
    int32_t symtablen, symtablocal;

    /*
     * Work out how many sections we will have. We have SHN_UNDEF,
     * then the flexible user sections, then the four fixed
     * sections `.comment', `.shstrtab', `.symtab' and `.strtab',
     * then optionally relocation sections for the user sections.
     */
    if (of_elf32.current_dfmt == &df_stabs)
        nsections = 8;
    else if (of_elf32.current_dfmt == &df_dwarf)
        nsections = 15;
    else
        nsections = 5;          /* SHN_UNDEF and the fixed ones */

    add_sectname("", ".comment");
    add_sectname("", ".shstrtab");
    add_sectname("", ".symtab");
    add_sectname("", ".strtab");
    for (i = 0; i < nsects; i++) {
        nsections++;            /* for the section itself */
        if (sects[i]->head) {
            nsections++;        /* for its relocations */
            add_sectname(".rel", sects[i]->name);
        }
    }

    if (of_elf32.current_dfmt == &df_stabs) {
        /* in case the debug information is wanted, just add these three sections... */
        add_sectname("", ".stab");
        add_sectname("", ".stabstr");
        add_sectname(".rel", ".stab");
    }

    else if (of_elf32.current_dfmt == &df_dwarf) {
        /* the dwarf debug standard specifies the following ten sections,
           not all of which are currently implemented,
           although all of them are defined. */
        #define debug_aranges (int32_t) (nsections-10)
        #define debug_info (int32_t) (nsections-7)
        #define debug_abbrev (int32_t) (nsections-5)
        #define debug_line (int32_t) (nsections-4)
        add_sectname("", ".debug_aranges");
        add_sectname(".rela", ".debug_aranges");
        add_sectname("", ".debug_pubnames");
        add_sectname("", ".debug_info");
        add_sectname(".rela", ".debug_info");
        add_sectname("", ".debug_abbrev");
        add_sectname("", ".debug_line");
        add_sectname(".rela", ".debug_line");
        add_sectname("", ".debug_frame");
        add_sectname("", ".debug_loc");
    }

    /*
     * Do the comment.
     */
    *comment = '\0';
    commlen =
        2 + sprintf(comment + 1, "The Netwide Assembler %s", NASM_VER);

    /*
     * Output the ELF header.
     */
    fwrite("\177ELF\1\1\1", 7, 1, elffp);
    fputc(elf_osabi, elffp);
    fputc(elf_abiver, elffp);
    fwrite("\0\0\0\0\0\0\0", 7, 1, elffp);
    fwriteint16_t(1, elffp);      /* ET_REL relocatable file */
    fwriteint16_t(3, elffp);      /* EM_386 processor ID */
    fwriteint32_t(1L, elffp);      /* EV_CURRENT file format version */
    fwriteint32_t(0L, elffp);      /* no entry point */
    fwriteint32_t(0L, elffp);      /* no program header table */
    fwriteint32_t(0x40L, elffp);   /* section headers straight after
                                 * ELF header plus alignment */
    fwriteint32_t(0L, elffp);      /* 386 defines no special flags */
    fwriteint16_t(0x34, elffp);   /* size of ELF header */
    fwriteint16_t(0, elffp);      /* no program header table, again */
    fwriteint16_t(0, elffp);      /* still no program header table */
    fwriteint16_t(0x28, elffp);   /* size of section header */
    fwriteint16_t(nsections, elffp);      /* number of sections */
    fwriteint16_t(nsects + 2, elffp);     /* string table section index for
                                         * section header table */
    fwriteint32_t(0L, elffp);      /* align to 0x40 bytes */
    fwriteint32_t(0L, elffp);
    fwriteint32_t(0L, elffp);

    /*
     * Build the symbol table and relocation tables.
     */
    symtab = elf_build_symtab(&symtablen, &symtablocal);
    for (i = 0; i < nsects; i++)
        if (sects[i]->head)
            sects[i]->rel = elf_build_reltab(&sects[i]->rellen,
                                             sects[i]->head);

    /*
     * Now output the section header table.
     */

    elf_foffs = 0x40 + 0x28 * nsections;
    align = ((elf_foffs + SEG_ALIGN_1) & ~SEG_ALIGN_1) - elf_foffs;
    elf_foffs += align;
    elf_nsect = 0;
    elf_sects = nasm_malloc(sizeof(*elf_sects) * nsections);

    elf_section_header(0, 0, 0, NULL, false, 0L, 0, 0, 0, 0);   /* SHN_UNDEF */
    scount = 1;                 /* needed for the stabs debugging to track the symtable section */
    p = shstrtab + 1;
    for (i = 0; i < nsects; i++) {
        elf_section_header(p - shstrtab, sects[i]->type, sects[i]->flags,
                           (sects[i]->type == SHT_PROGBITS ?
                            sects[i]->data : NULL), true,
                           sects[i]->len, 0, 0, sects[i]->align, 0);
        p += strlen(p) + 1;
        scount++;               /* dito */
    }
    elf_section_header(p - shstrtab, 1, 0, comment, false, (int32_t)commlen, 0, 0, 1, 0);  /* .comment */
    scount++;                   /* dito */
    p += strlen(p) + 1;
    elf_section_header(p - shstrtab, 3, 0, shstrtab, false, (int32_t)shstrtablen, 0, 0, 1, 0);     /* .shstrtab */
    scount++;                   /* dito */
    p += strlen(p) + 1;
    elf_section_header(p - shstrtab, 2, 0, symtab, true, symtablen, nsects + 4, symtablocal, 4, 16);    /* .symtab */
    symtabsection = scount;     /* now we got the symtab section index in the ELF file */
    p += strlen(p) + 1;
    elf_section_header(p - shstrtab, 3, 0, strs, true, strslen, 0, 0, 1, 0);    /* .strtab */
    for (i = 0; i < nsects; i++)
        if (sects[i]->head) {
            p += strlen(p) + 1;
            elf_section_header(p - shstrtab, 9, 0, sects[i]->rel, true,
                               sects[i]->rellen, nsects + 3, i + 1, 4, 8);
        }
    if (of_elf32.current_dfmt == &df_stabs) {
        /* for debugging information, create the last three sections
           which are the .stab , .stabstr and .rel.stab sections respectively */

        /* this function call creates the stab sections in memory */
        stabs32_generate();

        if ((stabbuf) && (stabstrbuf) && (stabrelbuf)) {
            p += strlen(p) + 1;
            elf_section_header(p - shstrtab, 1, 0, stabbuf, false, stablen,
                               nsections - 2, 0, 4, 12);

            p += strlen(p) + 1;
            elf_section_header(p - shstrtab, 3, 0, stabstrbuf, false,
                               stabstrlen, 0, 0, 4, 0);

            p += strlen(p) + 1;
            /* link -> symtable  info -> section to refer to */
            elf_section_header(p - shstrtab, 9, 0, stabrelbuf, false,
                               stabrellen, symtabsection, nsections - 3, 4,
                               8);
        }
    }
    else if (of_elf32.current_dfmt == &df_dwarf) {
            /* for dwarf debugging information, create the ten dwarf sections */

            /* this function call creates the dwarf sections in memory */
            if (dwarf_fsect) dwarf32_generate();

            p += strlen(p) + 1;
            elf_section_header(p - shstrtab, SHT_PROGBITS, 0, arangesbuf, false,
                               arangeslen, 0, 0, 1, 0);
            p += strlen(p) + 1;
            elf_section_header(p - shstrtab, SHT_RELA, 0, arangesrelbuf, false,
                               arangesrellen, symtabsection, debug_aranges, 1, 12);
            p += strlen(p) + 1;
            elf_section_header(p - shstrtab, SHT_PROGBITS, 0, pubnamesbuf, false,
                               pubnameslen, 0, 0, 1, 0);
            p += strlen(p) + 1;
            elf_section_header(p - shstrtab, SHT_PROGBITS, 0, infobuf, false,
                               infolen, 0, 0, 1, 0);
            p += strlen(p) + 1;
            elf_section_header(p - shstrtab, SHT_RELA, 0, inforelbuf, false,
                               inforellen, symtabsection, debug_info, 1, 12);
            p += strlen(p) + 1;
            elf_section_header(p - shstrtab, SHT_PROGBITS, 0, abbrevbuf, false,
                               abbrevlen, 0, 0, 1, 0);
            p += strlen(p) + 1;
            elf_section_header(p - shstrtab, SHT_PROGBITS, 0, linebuf, false,
                               linelen, 0, 0, 1, 0);
            p += strlen(p) + 1;
            elf_section_header(p - shstrtab, SHT_RELA, 0, linerelbuf, false,
                               linerellen, symtabsection, debug_line, 1, 12);
            p += strlen(p) + 1;
            elf_section_header(p - shstrtab, SHT_PROGBITS, 0, framebuf, false,
                               framelen, 0, 0, 8, 0);
            p += strlen(p) + 1;
            elf_section_header(p - shstrtab, SHT_PROGBITS, 0, locbuf, false,
                               loclen, 0, 0, 1, 0);

    }
    fwrite(align_str, align, 1, elffp);

    /*
     * Now output the sections.
     */
    elf_write_sections();

    nasm_free(elf_sects);
    saa_free(symtab);
}

static struct SAA *elf_build_symtab(int32_t *len, int32_t *local)
{
    struct SAA *s = saa_init(1L);
    struct Symbol *sym;
    uint8_t entry[16], *p;
    int i;

    *len = *local = 0;

    /*
     * First, an all-zeros entry, required by the ELF spec.
     */
    saa_wbytes(s, NULL, 16L);   /* null symbol table entry */
    *len += 16;
    (*local)++;

    /*
     * Next, an entry for the file name.
     */
    p = entry;
    WRITELONG(p, 1);            /* we know it's 1st entry in strtab */
    WRITELONG(p, 0);            /* no value */
    WRITELONG(p, 0);            /* no size either */
    WRITESHORT(p, STT_FILE);    /* type FILE */
    WRITESHORT(p, SHN_ABS);
    saa_wbytes(s, entry, 16L);
    *len += 16;
    (*local)++;

    /*
     * Now some standard symbols defining the segments, for relocation
     * purposes.
     */
    for (i = 1; i <= nsects; i++) {
        p = entry;
        WRITELONG(p, 0);        /* no symbol name */
        WRITELONG(p, 0);        /* offset zero */
        WRITELONG(p, 0);        /* size zero */
        WRITESHORT(p, STT_SECTION);       /* type, binding, and visibility */
        WRITESHORT(p, i);       /* section id */
        saa_wbytes(s, entry, 16L);
        *len += 16;
        (*local)++;
    }

    /*
     * Now the other local symbols.
     */
    saa_rewind(syms);
    while ((sym = saa_rstruct(syms))) {
        if (sym->type & SYM_GLOBAL)
            continue;
        p = entry;
        WRITELONG(p, sym->strpos);
        WRITELONG(p, sym->value);
        WRITELONG(p, sym->size);
        WRITECHAR(p, sym->type);        /* type and binding */
        WRITECHAR(p, sym->other);       /* visibility */
        WRITESHORT(p, sym->section);
        saa_wbytes(s, entry, 16L);
        *len += 16;
        (*local)++;
    }
     /*
      * dwarf needs symbols for debug sections
      * which are relocation targets.
      */  
//*** fix for 32 bit
     if (of_elf32.current_dfmt == &df_dwarf) {
        dwarf_infosym = *local;
        p = entry;
        WRITELONG(p, 0);        /* no symbol name */
        WRITELONG(p, (uint32_t) 0);        /* offset zero */
        WRITELONG(p, (uint32_t) 0);        /* size zero */
        WRITESHORT(p, STT_SECTION);       /* type, binding, and visibility */
        WRITESHORT(p, debug_info);       /* section id */
        saa_wbytes(s, entry, 16L);
        *len += 16;
        (*local)++;
        dwarf_abbrevsym = *local;
        p = entry;
        WRITELONG(p, 0);        /* no symbol name */
        WRITELONG(p, (uint32_t) 0);        /* offset zero */
        WRITELONG(p, (uint32_t) 0);        /* size zero */
        WRITESHORT(p, STT_SECTION);       /* type, binding, and visibility */
        WRITESHORT(p, debug_abbrev);       /* section id */
        saa_wbytes(s, entry, 16L);
        *len += 16;
        (*local)++;
        dwarf_linesym = *local;
        p = entry;
        WRITELONG(p, 0);        /* no symbol name */
        WRITELONG(p, (uint32_t) 0);        /* offset zero */
        WRITELONG(p, (uint32_t) 0);        /* size zero */
        WRITESHORT(p, STT_SECTION);       /* type, binding, and visibility */
        WRITESHORT(p, debug_line);       /* section id */
        saa_wbytes(s, entry, 16L);
        *len += 16;
        (*local)++;
     }

    /*
     * Now the global symbols.
     */
    saa_rewind(syms);
    while ((sym = saa_rstruct(syms))) {
        if (!(sym->type & SYM_GLOBAL))
            continue;
        p = entry;
        WRITELONG(p, sym->strpos);
        WRITELONG(p, sym->value);
        WRITELONG(p, sym->size);
        WRITECHAR(p, sym->type);        /* type and binding */
        WRITECHAR(p, sym->other);       /* visibility */
        WRITESHORT(p, sym->section);
        saa_wbytes(s, entry, 16L);
        *len += 16;
    }

    return s;
}

static struct SAA *elf_build_reltab(int32_t *len, struct Reloc *r)
{
    struct SAA *s;
    uint8_t *p, entry[8];

    if (!r)
        return NULL;

    s = saa_init(1L);
    *len = 0;

    while (r) {
        int32_t sym = r->symbol;

        if (sym >= GLOBAL_TEMP_BASE)
        {
           if (of_elf32.current_dfmt == &df_dwarf)
              sym += -GLOBAL_TEMP_BASE + (nsects + 5) + nlocals;
           else   sym += -GLOBAL_TEMP_BASE + (nsects + 2) + nlocals;
        }

        p = entry;
        WRITELONG(p, r->address);
        WRITELONG(p, (sym << 8) + r->type);
        saa_wbytes(s, entry, 8L);
        *len += 8;

        r = r->next;
    }

    return s;
}

static void elf_section_header(int name, int type, int flags,
                               void *data, bool is_saa, int32_t datalen,
                               int link, int info, int align, int eltsize)
{
    elf_sects[elf_nsect].data = data;
    elf_sects[elf_nsect].len = datalen;
    elf_sects[elf_nsect].is_saa = is_saa;
    elf_nsect++;

    fwriteint32_t((int32_t)name, elffp);
    fwriteint32_t((int32_t)type, elffp);
    fwriteint32_t((int32_t)flags, elffp);
    fwriteint32_t(0L, elffp);      /* no address, ever, in object files */
    fwriteint32_t(type == 0 ? 0L : elf_foffs, elffp);
    fwriteint32_t(datalen, elffp);
    if (data)
        elf_foffs += (datalen + SEG_ALIGN_1) & ~SEG_ALIGN_1;
    fwriteint32_t((int32_t)link, elffp);
    fwriteint32_t((int32_t)info, elffp);
    fwriteint32_t((int32_t)align, elffp);
    fwriteint32_t((int32_t)eltsize, elffp);
}

static void elf_write_sections(void)
{
    int i;
    for (i = 0; i < elf_nsect; i++)
        if (elf_sects[i].data) {
            int32_t len = elf_sects[i].len;
            int32_t reallen = (len + SEG_ALIGN_1) & ~SEG_ALIGN_1;
            int32_t align = reallen - len;
            if (elf_sects[i].is_saa)
                saa_fpwrite(elf_sects[i].data, elffp);
            else
                fwrite(elf_sects[i].data, len, 1, elffp);
            fwrite(align_str, align, 1, elffp);
        }
}

static void elf_sect_write(struct Section *sect,
                           const uint8_t *data, uint32_t len)
{
    saa_wbytes(sect->data, data, len);
    sect->len += len;
}

static int32_t elf_segbase(int32_t segment)
{
    return segment;
}

static int elf_directive(char *directive, char *value, int pass)
{
    bool err;
    int64_t n;
    char *p;

    if (!strcmp(directive, "osabi")) {
	if (pass == 2)
	    return 1;		/* ignore in pass 2 */

	n = readnum(value, &err);
	if (err) {
	    error(ERR_NONFATAL, "`osabi' directive requires a parameter");
	    return 1;
	}
	if (n < 0 || n > 255) {
	    error(ERR_NONFATAL, "valid osabi numbers are 0 to 255");
	    return 1;
	}
	elf_osabi  = n;
	elf_abiver = 0;

	if ((p = strchr(value,',')) == NULL)
	    return 1;

	n = readnum(p+1, &err);
	if (err || n < 0 || n > 255) {
	    error(ERR_NONFATAL, "invalid ABI version number (valid: 0 to 255)");
	    return 1;
	}
	
	elf_abiver = n;
	return 1;
    }
	
    return 0;
}

static void elf_filename(char *inname, char *outname, efunc error)
{
    strcpy(elf_module, inname);
    standard_extension(inname, outname, ".o", error);
}

static const char *elf_stdmac[] = {
    "%define __SECT__ [section .text]",
    "%macro __NASM_CDecl__ 1",
    "%define $_%1 $%1",
    "%endmacro",
    "%macro osabi 1+.nolist",
    "[osabi %1]",
    "%endmacro",
    NULL
};
static int elf_set_info(enum geninfo type, char **val)
{
    (void)type;
    (void)val;
    return 0;
}
static struct dfmt df_dwarf = {
    "elf32 (X86_64) dwarf debug format for Linux",
    "dwarf",
    debug32_init,
    dwarf32_linenum,
    debug32_deflabel,
    debug32_directive,
    debug32_typevalue,
    dwarf32_output,
    dwarf32_cleanup
};
static struct dfmt df_stabs = {
    "ELF32 (i386) stabs debug format for Linux",
    "stabs",
    debug32_init,
    stabs32_linenum,
    debug32_deflabel,
    debug32_directive,
    debug32_typevalue,
    stabs32_output,
    stabs32_cleanup
};

struct dfmt *elf32_debugs_arr[3] = { &df_stabs, &df_dwarf, NULL };

struct ofmt of_elf32 = {
    "ELF32 (i386) object files (e.g. Linux)",
    "elf32",
    NULL,
    elf32_debugs_arr,
    &null_debug_form,
    elf_stdmac,
    elf_init,
    elf_set_info,
    elf_out,
    elf_deflabel,
    elf_section_names,
    elf_segbase,
    elf_directive,
    elf_filename,
    elf_cleanup
};

struct ofmt of_elf = {
    "ELF (short name for ELF32) ",
    "elf",
    NULL,
    elf32_debugs_arr,
    &null_debug_form,
    elf_stdmac,
    elf_init,
    elf_set_info,
    elf_out,
    elf_deflabel,
    elf_section_names,
    elf_segbase,
    elf_directive,
    elf_filename,
    elf_cleanup
};
/* again, the stabs debugging stuff (code) */

void debug32_init(struct ofmt *of, void *id, FILE * fp, efunc error)
{
    (void)of;
    (void)id;
    (void)fp;
    (void)error;
}

void stabs32_linenum(const char *filename, int32_t linenumber, int32_t segto)
{
    (void)segto;

    if (!stabs_filename) {
        stabs_filename = (char *)nasm_malloc(strlen(filename) + 1);
        strcpy(stabs_filename, filename);
    } else {
        if (strcmp(stabs_filename, filename)) {
            /* yep, a memory leak...this program is one-shot anyway, so who cares...
               in fact, this leak comes in quite handy to maintain a list of files
               encountered so far in the symbol lines... */

            /* why not nasm_free(stabs_filename); we're done with the old one */

            stabs_filename = (char *)nasm_malloc(strlen(filename) + 1);
            strcpy(stabs_filename, filename);
        }
    }
    debug_immcall = 1;
    currentline = linenumber;
}

void debug32_deflabel(char *name, int32_t segment, int64_t offset, int is_global,
                    char *special)
{
   (void)name;
   (void)segment;
   (void)offset;
   (void)is_global;
   (void)special;
}

void debug32_directive(const char *directive, const char *params)
{
   (void)directive;
   (void)params;
}

void debug32_typevalue(int32_t type)
{
    int32_t stype, ssize;
    switch (TYM_TYPE(type)) {
        case TY_LABEL:
            ssize = 0;
            stype = STT_NOTYPE;
            break;
        case TY_BYTE:
            ssize = 1;
            stype = STT_OBJECT;
            break;
        case TY_WORD:
            ssize = 2;
            stype = STT_OBJECT;
            break;
        case TY_DWORD:
            ssize = 4;
            stype = STT_OBJECT;
            break;
        case TY_FLOAT:
            ssize = 4;
            stype = STT_OBJECT;
            break;
        case TY_QWORD:
            ssize = 8;
            stype = STT_OBJECT;
            break;
        case TY_TBYTE:
            ssize = 10;
            stype = STT_OBJECT;
            break;
        case TY_OWORD:
            ssize = 8;
            stype = STT_OBJECT;
            break;
        case TY_COMMON:
            ssize = 0;
            stype = STT_COMMON;
            break;
        case TY_SEG:
            ssize = 0;
            stype = STT_SECTION;
            break;
        case TY_EXTERN:
            ssize = 0;
            stype = STT_NOTYPE;
            break;
        case TY_EQU:
            ssize = 0;
            stype = STT_NOTYPE;
            break;
        default:
            ssize = 0;
            stype = STT_NOTYPE;
            break;
    }
    if (stype == STT_OBJECT && lastsym && !lastsym->type) {
        lastsym->size = ssize;
        lastsym->type = stype;
    }
}

void stabs32_output(int type, void *param)
{
    struct symlininfo *s;
    struct linelist *el;
    if (type == TY_STABSSYMLIN) {
        if (debug_immcall) {
            s = (struct symlininfo *)param;
            if (!(sects[s->section]->flags & SHF_EXECINSTR))
                return;         /* we are only interested in the text stuff */
            numlinestabs++;
            el = (struct linelist *)nasm_malloc(sizeof(struct linelist));
            el->info.offset = s->offset;
            el->info.section = s->section;
            el->info.name = s->name;
            el->line = currentline;
            el->filename = stabs_filename;
            el->next = 0;
            if (stabslines) {
                stabslines->last->next = el;
                stabslines->last = el;
            } else {
                stabslines = el;
                stabslines->last = el;
            }
        }
    }
    debug_immcall = 0;
}

#define WRITE_STAB(p,n_strx,n_type,n_other,n_desc,n_value) \
  do {\
    WRITELONG(p,n_strx); \
    WRITECHAR(p,n_type); \
    WRITECHAR(p,n_other); \
    WRITESHORT(p,n_desc); \
    WRITELONG(p,n_value); \
  } while (0)

/* for creating the .stab , .stabstr and .rel.stab sections in memory */

void stabs32_generate(void)
{
    int i, numfiles, strsize, numstabs = 0, currfile, mainfileindex;
    uint8_t *sbuf, *ssbuf, *rbuf, *sptr, *rptr;
    char **allfiles;
    int *fileidx;

    struct linelist *ptr;

    ptr = stabslines;

    allfiles = (char **)nasm_malloc(numlinestabs * sizeof(char *));
    for (i = 0; i < numlinestabs; i++)
        allfiles[i] = 0;
    numfiles = 0;
    while (ptr) {
        if (numfiles == 0) {
            allfiles[0] = ptr->filename;
            numfiles++;
        } else {
            for (i = 0; i < numfiles; i++) {
                if (!strcmp(allfiles[i], ptr->filename))
                    break;
            }
            if (i >= numfiles) {
                allfiles[i] = ptr->filename;
                numfiles++;
            }
        }
        ptr = ptr->next;
    }
    strsize = 1;
    fileidx = (int *)nasm_malloc(numfiles * sizeof(int));
    for (i = 0; i < numfiles; i++) {
        fileidx[i] = strsize;
        strsize += strlen(allfiles[i]) + 1;
    }
    mainfileindex = 0;
    for (i = 0; i < numfiles; i++) {
        if (!strcmp(allfiles[i], elf_module)) {
            mainfileindex = i;
            break;
        }
    }

    /* worst case size of the stab buffer would be:
       the sourcefiles changes each line, which would mean 1 SOL, 1 SYMLIN per line
     */
    sbuf =
        (uint8_t *)nasm_malloc((numlinestabs * 2 + 3) *
                                     sizeof(struct stabentry));

    ssbuf = (uint8_t *)nasm_malloc(strsize);

    rbuf = (uint8_t *)nasm_malloc(numlinestabs * 8 * (2 + 3));
    rptr = rbuf;

    for (i = 0; i < numfiles; i++) {
        strcpy((char *)ssbuf + fileidx[i], allfiles[i]);
    }
    ssbuf[0] = 0;

    stabstrlen = strsize;       /* set global variable for length of stab strings */

    sptr = sbuf;
    ptr = stabslines;
    numstabs = 0;

    if (ptr) {
        /* this is the first stab, its strx points to the filename of the
        the source-file, the n_desc field should be set to the number
        of remaining stabs
        */
        WRITE_STAB(sptr, fileidx[0], 0, 0, 0, strlen(allfiles[0] + 12));

        /* this is the stab for the main source file */
        WRITE_STAB(sptr, fileidx[mainfileindex], N_SO, 0, 0, 0);

        /* relocation table entry */

        /* Since the symbol table has two entries before */
        /* the section symbols, the index in the info.section */
        /* member must be adjusted by adding 2 */

        WRITELONG(rptr, (sptr - sbuf) - 4);
        WRITELONG(rptr, ((ptr->info.section + 2) << 8) | R_386_32);

        numstabs++;
        currfile = mainfileindex;
    }

    while (ptr) {
        if (strcmp(allfiles[currfile], ptr->filename)) {
            /* oops file has changed... */
            for (i = 0; i < numfiles; i++)
                if (!strcmp(allfiles[i], ptr->filename))
                    break;
            currfile = i;
            WRITE_STAB(sptr, fileidx[currfile], N_SOL, 0, 0,
                       ptr->info.offset);
            numstabs++;

            /* relocation table entry */
            WRITELONG(rptr, (sptr - sbuf) - 4);
            WRITELONG(rptr, ((ptr->info.section + 2) << 8) | R_386_32);
        }

        WRITE_STAB(sptr, 0, N_SLINE, 0, ptr->line, ptr->info.offset);
        numstabs++;

        /* relocation table entry */

        WRITELONG(rptr, (sptr - sbuf) - 4);
        WRITELONG(rptr, ((ptr->info.section + 2) << 8) | R_386_32);

        ptr = ptr->next;

    }

    ((struct stabentry *)sbuf)->n_desc = numstabs;

    nasm_free(allfiles);
    nasm_free(fileidx);

    stablen = (sptr - sbuf);
    stabrellen = (rptr - rbuf);
    stabrelbuf = rbuf;
    stabbuf = sbuf;
    stabstrbuf = ssbuf;
}

void stabs32_cleanup(void)
{
    struct linelist *ptr, *del;
    if (!stabslines)
        return;
    ptr = stabslines;
    while (ptr) {
        del = ptr;
        ptr = ptr->next;
        nasm_free(del);
    }
    if (stabbuf)
        nasm_free(stabbuf);
    if (stabrelbuf)
        nasm_free(stabrelbuf);
    if (stabstrbuf)
        nasm_free(stabstrbuf);
}
/* dwarf routines */


void dwarf32_linenum(const char *filename, int32_t linenumber, int32_t segto)
{
    (void)segto;
    dwarf32_findfile(filename);
    debug_immcall = 1;
    currentline = linenumber;
}

/* called from elf_out with type == TY_DEBUGSYMLIN */
void dwarf32_output(int type, void *param)
{
  int ln, aa, inx, maxln, soc;
  struct symlininfo *s;
  struct SAA *plinep;

  (void)type;

  s = (struct symlininfo *)param;
   /* line number info is only gathered for executable sections */
   if (!(sects[s->section]->flags & SHF_EXECINSTR))
     return;
  /* Check if section index has changed */
  if (!(dwarf_csect && (dwarf_csect->section) == (s->section)))
  {
     dwarf32_findsect(s->section);
  }
  /* do nothing unless line or file has changed */
  if (debug_immcall)
  {
    ln = currentline - dwarf_csect->line;
    aa = s->offset - dwarf_csect->offset;
    inx = dwarf_clist->line;
    plinep = dwarf_csect->psaa;
    /* check for file change */
    if (!(inx == dwarf_csect->file))
    {
       saa_write8(plinep,DW_LNS_set_file);
       saa_write8(plinep,inx);
       dwarf_csect->file = inx;
    }
    /* check for line change */
    if (ln)
    {
       /* test if in range of special op code */
       maxln = line_base + line_range;
       soc = (ln - line_base) + (line_range * aa) + opcode_base;
       if (ln >= line_base && ln < maxln && soc < 256)
       {
          saa_write8(plinep,soc);
       }
       else
       {
          if (ln)
          {
          saa_write8(plinep,DW_LNS_advance_line);
          saa_wleb128s(plinep,ln);
          }
          if (aa)
          {
          saa_write8(plinep,DW_LNS_advance_pc);
          saa_wleb128u(plinep,aa);
          }
       }
       dwarf_csect->line = currentline;
       dwarf_csect->offset = s->offset;
    }
    /* show change handled */
    debug_immcall = 0;
  }
}


void dwarf32_generate(void)
{
    static const char nasm_signature[] = "NASM " NASM_VER;
    uint8_t *pbuf;
    int indx;
    struct linelist *ftentry;
    struct SAA *paranges, *ppubnames, *pinfo, *pabbrev, *plines, *plinep;
    struct SAA *parangesrel, *plinesrel, *pinforel;
    struct sectlist *psect;
    size_t saalen, linepoff, totlen, highaddr;

    /* write epilogues for each line program range */
    /* and build aranges section */
    paranges = saa_init(1L);
    parangesrel = saa_init(1L);
    saa_write16(paranges,2);		/* dwarf version */
    saa_write32(parangesrel, paranges->datalen+4);
    saa_write32(parangesrel, (dwarf_infosym << 8) +  R_386_32); /* reloc to info */
    saa_write32(parangesrel, 0);
    saa_write32(paranges,0);		/* offset into info */
    saa_write8(paranges,4);		/* pointer size */
    saa_write8(paranges,0);		/* not segmented */
    saa_write32(paranges,0);		/* padding */
    /* iterate though sectlist entries */
     psect = dwarf_fsect;
     totlen = 0;
     highaddr = 0;
     for (indx = 0; indx < dwarf_nsections; indx++)
     {
         plinep = psect->psaa;
         /* Line Number Program Epilogue */
         saa_write8(plinep,2);			/* std op 2 */
         saa_write8(plinep,(sects[psect->section]->len)-psect->offset);
         saa_write8(plinep,DW_LNS_extended_op);
         saa_write8(plinep,1);			/* operand length */
         saa_write8(plinep,DW_LNE_end_sequence);
         totlen += plinep->datalen;
         /* range table relocation entry */
         saa_write32(parangesrel, paranges->datalen + 4);
         saa_write32(parangesrel, ((uint32_t) (psect->section + 2) << 8) +  R_386_32);
         saa_write32(parangesrel, (uint32_t) 0);
         /* range table entry */
         saa_write32(paranges,0x0000);		/* range start */
         saa_write32(paranges,sects[psect->section]->len);	/* range length */
         highaddr += sects[psect->section]->len;
         /* done with this entry */
         psect = psect->next;
     }
    saa_write32(paranges,0);		/* null address */
    saa_write32(paranges,0);		/* null length */
    saalen = paranges->datalen;
    arangeslen = saalen + 4;
    arangesbuf = pbuf = nasm_malloc(arangeslen);
    WRITELONG(pbuf,saalen);			/* initial length */
    saa_rnbytes(paranges, pbuf, saalen);
    saa_free(paranges);

    /* build rela.aranges section */
    arangesrellen = saalen = parangesrel->datalen;
    arangesrelbuf = pbuf = nasm_malloc(arangesrellen); 
    saa_rnbytes(parangesrel, pbuf, saalen);
    saa_free(parangesrel);

    /* build pubnames section */
    ppubnames = saa_init(1L);
    saa_write16(ppubnames,3);			/* dwarf version */
    saa_write32(ppubnames,0);			/* offset into info */
    saa_write32(ppubnames,0);			/* space used in info */
    saa_write32(ppubnames,0);			/* end of list */
    saalen = ppubnames->datalen;
    pubnameslen = saalen + 4;
    pubnamesbuf = pbuf = nasm_malloc(pubnameslen);
    WRITELONG(pbuf,saalen);	/* initial length */
    saa_rnbytes(ppubnames, pbuf, saalen);
    saa_free(ppubnames);

    /* build info section */
    pinfo = saa_init(1L);
    pinforel = saa_init(1L);
    saa_write16(pinfo,2);			/* dwarf version */
    saa_write32(pinforel, pinfo->datalen + 4);
    saa_write32(pinforel, (dwarf_abbrevsym << 8) +  R_386_32); /* reloc to abbrev */
    saa_write32(pinforel, 0);
    saa_write32(pinfo,0);			/* offset into abbrev */
    saa_write8(pinfo,4);			/* pointer size */
    saa_write8(pinfo,1);			/* abbrviation number LEB128u */
    saa_write32(pinforel, pinfo->datalen + 4);
    saa_write32(pinforel, ((dwarf_fsect->section + 2) << 8) +  R_386_32);
    saa_write32(pinforel, 0);
    saa_write32(pinfo,0);			/* DW_AT_low_pc */
    saa_write32(pinforel, pinfo->datalen + 4);
    saa_write32(pinforel, ((dwarf_fsect->section + 2) << 8) +  R_386_32);
    saa_write32(pinforel, 0);
    saa_write32(pinfo,highaddr);		/* DW_AT_high_pc */
    saa_write32(pinforel, pinfo->datalen + 4);
    saa_write32(pinforel, (dwarf_linesym << 8) +  R_386_32); /* reloc to line */
    saa_write32(pinforel, 0);
    saa_write32(pinfo,0);			/* DW_AT_stmt_list */
    saa_wbytes(pinfo, elf_module, strlen(elf_module)+1);
    saa_wbytes(pinfo, nasm_signature, strlen(nasm_signature)+1);
    saa_write16(pinfo,DW_LANG_Mips_Assembler);
    saa_write8(pinfo,2);			/* abbrviation number LEB128u */
    saa_write32(pinforel, pinfo->datalen + 4);
    saa_write32(pinforel, ((dwarf_fsect->section + 2) << 8) +  R_386_32);
    saa_write32(pinforel, 0);
    saa_write32(pinfo,0);			/* DW_AT_low_pc */
    saa_write32(pinfo,0);			/* DW_AT_frame_base */
    saa_write8(pinfo,0);			/* end of entries */
    saalen = pinfo->datalen;
    infolen = saalen + 4;
    infobuf = pbuf = nasm_malloc(infolen);
    WRITELONG(pbuf,saalen);		/* initial length */
    saa_rnbytes(pinfo, pbuf, saalen);
    saa_free(pinfo);

    /* build rela.info section */
    inforellen = saalen = pinforel->datalen;
    inforelbuf = pbuf = nasm_malloc(inforellen);
    saa_rnbytes(pinforel, pbuf, saalen);
    saa_free(pinforel); 

    /* build abbrev section */
    pabbrev = saa_init(1L);
    saa_write8(pabbrev,1);			/* entry number LEB128u */
    saa_write8(pabbrev,DW_TAG_compile_unit);	/* tag LEB128u */
    saa_write8(pabbrev,1);			/* has children */
    /* the following attributes and forms are all LEB128u values */
    saa_write8(pabbrev,DW_AT_low_pc);
    saa_write8(pabbrev,DW_FORM_addr);
    saa_write8(pabbrev,DW_AT_high_pc);
    saa_write8(pabbrev,DW_FORM_addr);
    saa_write8(pabbrev,DW_AT_stmt_list);
    saa_write8(pabbrev,DW_FORM_data4);
    saa_write8(pabbrev,DW_AT_name);
    saa_write8(pabbrev,DW_FORM_string);
    saa_write8(pabbrev,DW_AT_producer);
    saa_write8(pabbrev,DW_FORM_string);
    saa_write8(pabbrev,DW_AT_language);
    saa_write8(pabbrev,DW_FORM_data2);
    saa_write16(pabbrev,0);			/* end of entry */
    /* LEB128u usage same as above */
    saa_write8(pabbrev,2);			/* entry number */
    saa_write8(pabbrev,DW_TAG_subprogram);
    saa_write8(pabbrev,0);			/* no children */
    saa_write8(pabbrev,DW_AT_low_pc);
    saa_write8(pabbrev,DW_FORM_addr);
    saa_write8(pabbrev,DW_AT_frame_base);
    saa_write8(pabbrev,DW_FORM_data4);
    saa_write16(pabbrev,0);			/* end of entry */
    abbrevlen = saalen = pabbrev->datalen;
    abbrevbuf = pbuf = nasm_malloc(saalen);
    saa_rnbytes(pabbrev, pbuf, saalen);
    saa_free(pabbrev);

    /* build line section */
    /* prolog */
    plines = saa_init(1L);
    saa_write8(plines,1);			/* Minimum Instruction Length */
    saa_write8(plines,1);			/* Initial value of 'is_stmt' */
    saa_write8(plines,line_base);		/* Line Base */
    saa_write8(plines,line_range);	/* Line Range */
    saa_write8(plines,opcode_base);	/* Opcode Base */
    /* standard opcode lengths (# of LEB128u operands) */
    saa_write8(plines,0);			/* Std opcode 1 length */
    saa_write8(plines,1);			/* Std opcode 2 length */
    saa_write8(plines,1);			/* Std opcode 3 length */
    saa_write8(plines,1);			/* Std opcode 4 length */
    saa_write8(plines,1);			/* Std opcode 5 length */
    saa_write8(plines,0);			/* Std opcode 6 length */
    saa_write8(plines,0);			/* Std opcode 7 length */
    saa_write8(plines,0);			/* Std opcode 8 length */
    saa_write8(plines,1);			/* Std opcode 9 length */
    saa_write8(plines,0);			/* Std opcode 10 length */
    saa_write8(plines,0);			/* Std opcode 11 length */
    saa_write8(plines,1);			/* Std opcode 12 length */
    /* Directory Table */ 
    saa_write8(plines,0);			/* End of table */
    /* File Name Table */
    ftentry = dwarf_flist;
    for (indx = 0;indx<dwarf_numfiles;indx++)
    {
      saa_wbytes(plines, ftentry->filename, (int32_t)(strlen(ftentry->filename) + 1));
      saa_write8(plines,0);			/* directory  LEB128u */
      saa_write8(plines,0);			/* time LEB128u */
      saa_write8(plines,0);			/* size LEB128u */
      ftentry = ftentry->next;
    }
    saa_write8(plines,0);			/* End of table */
    linepoff = plines->datalen;
    linelen = linepoff + totlen + 10;
    linebuf = pbuf = nasm_malloc(linelen);
    WRITELONG(pbuf,linelen-4);		/* initial length */
    WRITESHORT(pbuf,3);			/* dwarf version */
    WRITELONG(pbuf,linepoff);		/* offset to line number program */
    /* write line header */
    saalen = linepoff;
    saa_rnbytes(plines, pbuf, saalen);   /* read a given no. of bytes */
    pbuf += linepoff;
    saa_free(plines);
    /* concatonate line program ranges */
    linepoff += 13;
    plinesrel = saa_init(1L);
    psect = dwarf_fsect;
    for (indx = 0; indx < dwarf_nsections; indx++)
    {
         saa_write32(plinesrel, linepoff);
         saa_write32(plinesrel, ((uint32_t) (psect->section + 2) << 8) +  R_386_32);
         saa_write32(plinesrel, (uint32_t) 0);
         plinep = psect->psaa;
         saalen = plinep->datalen;
         saa_rnbytes(plinep, pbuf, saalen);
         pbuf += saalen;
         linepoff += saalen;
         saa_free(plinep);
         /* done with this entry */
         psect = psect->next;
    }


    /* build rela.lines section */
    linerellen =saalen = plinesrel->datalen;
    linerelbuf = pbuf = nasm_malloc(linerellen); 
    saa_rnbytes(plinesrel, pbuf, saalen);
    saa_free(plinesrel);

    /* build frame section */
    framelen = 4;
    framebuf = pbuf = nasm_malloc(framelen);
    WRITELONG(pbuf,framelen-4);		/* initial length */

    /* build loc section */
    loclen = 16;
    locbuf = pbuf = nasm_malloc(loclen);
    WRITELONG(pbuf,0);		/* null  beginning offset */
    WRITELONG(pbuf,0);		/* null  ending offset */
}

void dwarf32_cleanup(void)
{
    if (arangesbuf)
        nasm_free(arangesbuf);
    if (arangesrelbuf)
        nasm_free(arangesrelbuf);
    if (pubnamesbuf)
        nasm_free(pubnamesbuf);
    if (infobuf)
        nasm_free(infobuf);
    if (inforelbuf)
        nasm_free(inforelbuf);
    if (abbrevbuf)
        nasm_free(abbrevbuf);
    if (linebuf)
        nasm_free(linebuf);
    if (linerelbuf)
        nasm_free(linerelbuf);
    if (framebuf)
        nasm_free(framebuf);
    if (locbuf)
        nasm_free(locbuf);
}
void dwarf32_findfile(const char * fname)
{
   int finx;
   struct linelist *match;

   /* return if fname is current file name */
   if (dwarf_clist && !(strcmp(fname, dwarf_clist->filename))) return;
   /* search for match */
   else 
   {
     match = 0;
     if (dwarf_flist)
     {
       match = dwarf_flist;
       for (finx = 0; finx < dwarf_numfiles; finx++)
       {
         if (!(strcmp(fname, match->filename)))
         {
	   dwarf_clist = match;
           return;
         }
       }
     }
     /* add file name to end of list */
     dwarf_clist =  (struct linelist *)nasm_malloc(sizeof(struct linelist));
     dwarf_numfiles++;
     dwarf_clist->line = dwarf_numfiles;
     dwarf_clist->filename = nasm_malloc(strlen(fname) + 1);
     strcpy(dwarf_clist->filename,fname);
     dwarf_clist->next = 0;
     /* if first entry */
     if (!dwarf_flist)
     {
       dwarf_flist = dwarf_elist = dwarf_clist;
       dwarf_clist->last = 0;
     }
     /* chain to previous entry */
     else
     {
       dwarf_elist->next = dwarf_clist;
       dwarf_elist = dwarf_clist;
     }
   }
}
/*  */
void dwarf32_findsect(const int index)
{
   int sinx;
   struct sectlist *match;
   struct SAA *plinep;
   /* return if index is current section index */
   if (dwarf_csect && (dwarf_csect->section == index))
   {
      return;
   }
   /* search for match */
   else 
   {
     match = 0;
     if (dwarf_fsect)
     {
       match = dwarf_fsect;
       for (sinx = 0; sinx < dwarf_nsections; sinx++)
       {
         if ((match->section == index))
         {
	   dwarf_csect = match;
           return;
         }
        match = match->next;
       }
     }
     /* add entry to end of list */
     dwarf_csect =  (struct sectlist *)nasm_malloc(sizeof(struct sectlist));
     dwarf_nsections++;
     dwarf_csect->psaa = plinep = saa_init(1L);
     dwarf_csect->line = 1;
     dwarf_csect->offset = 0;
     dwarf_csect->file = 1;
     dwarf_csect->section = index;
     dwarf_csect->next = 0;
     /* set relocatable address at start of line program */
     saa_write8(plinep,DW_LNS_extended_op);
     saa_write8(plinep,5);			/* operand length */
     saa_write8(plinep,DW_LNE_set_address);
     saa_write32(plinep,0);		/* Start Address */
     /* if first entry */
     if (!dwarf_fsect)
     {
       dwarf_fsect = dwarf_esect = dwarf_csect;
       dwarf_csect->last = 0;
     }
     /* chain to previous entry */
     else
     {
       dwarf_esect->next = dwarf_csect;
       dwarf_esect = dwarf_csect;
     }
   }
}

#endif                          /* OF_ELF */
