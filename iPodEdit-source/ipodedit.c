/*
   ipodedit.c
   Available at http://austinche.name/ipod/
   Dumps and writes pics, text, and other resources in an iPod firmware
   Thanks to ipodwizard for the inspiration.

   Copyright (c) 2004-2006 Austin Che

   This program is released under the terms of the GNU General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   Version history (major changes):
   2004/12/12: Initial version to dump and change pictures
   2004/12/13: Big endian support (thanks Josh)
   2004/12/14: Text editing
   2004/12/16: Add checksum code for all ipod versions (thanks Bernard)
   2004/12/17: Clean up code to compile with no warnings
   2004/12/19: Dump fonts
   2004/12/21: Add compile time options to reduce verbosity
   2004/12/22: Extract firmwares from updater/file
   2004/12/23: Add color bitmap support (PPM files)
   2004/12/24: Dump all resource information
   2004/12/31: Change interface to support editing resources generically
   2005/01/01: Parse comments in PGM/PPM files (thanks Kees)
   2005/01/05: Reorganize/merge commands. Add short form for commands
   2005/01/06: Write back firmwares to updater/file (thanks Lincoln)
   2005/01/08: Add support for bootloader images (thanks ipodwizard)
   2005/01/19: Print out resource info when extracting
   2005/03/06: Add in command to decode a single resource file
   2005/03/07: Add mapping commands to allow for portable file names
   2005/04/11: Support iPodLinux firmware and compiling for iPod
   2005/04/12: Handle menu and type resources in a semi-friendly form
   2005/07/03: Add big-endian support for fonts (thanks Adam)
   2005/11/06: Add support for reading color pictures from nano and video

*/

// ---------------------------------------------------------------------------------
// Standard includes

#include <stdio.h>
#include <string.h>
#include <stdlib.h>             // exit()
#include <stdint.h>             // uint32_t uint16_t
#include <sys/stat.h>           // mkdir()
#include <libgen.h>             // basename()
#include <stdarg.h>             // va_list
#include <ctype.h>              // isascii() isspace()
#include <limits.h>             // PATH_MAX
#include <sys/param.h>          // PATH_MAX needed for compiling for ipod (ARM)

// ---------------------------------------------------------------------------------
// Compile time options

#define DIR_PERMISSIONS 0744
#define RAW_RESOURCES_DIR "ipod.raw"
#define DATA_DIR "ipod"
#define MAP_DIR "ipod.map"
#define FIRMWARE_DIR "ipod-fw"
#define RESOURCE_INFO_FILE "ipod-resources.txt"
#define RESOURCE_MAP_FILE "resource.map"

#define DEBUG 0

// these are very verbose, which most people probably don't want
#define PRINT_BASIC_FONT_INFO 0
#define PRINT_ALL_FONT_INFO 0

// ---------------------------------------------------------------------------------

// to handle all bitmaps (fonts and other pictures) similarly
// This can copy from one type of struct to another with compatible names
#define COPY_BITMAP(x, y) do { \
    (x)->height = (y)->height;\
    (x)->width = (y)->width;\
    (x)->bytes_per_row = (y)->bytes_per_row;\
    (x)->depth = (y)->depth;\
    (x)->length = (y)->length;\
} while(0)

struct bitmap_info
{
    int height;                 // number of rows
    int width;                  // number of columns
    int bytes_per_row;          // number of bytes used per row (incl. padding)
    int depth;                  // bits per pixel
    int length;                 // height * bytes_per_row
    int original_depth;         // bits per pixel in original image

    int color_flipped;          // whether 2-byte colors are flipped
};

struct picture_header
{
    uint32_t z1;                // always 0 (not always! not on color firmware)
    uint32_t z2;                // always 0
    uint32_t height;
    uint32_t width;
    uint16_t bytes_per_row;
    uint16_t depth;
    uint32_t z3;                // always 0 (not always! on nano(?) firmware, seems to indicate color bytes are flipped)
    uint32_t length;
};
#define FIX_PICTURE_HEADER(x) do { \
    flip32(&(x)->z1, &(x)->z2, &(x)->height, &(x)->width,\
           &(x)->z3, &(x)->length, NULL); \
    flip16(&(x)->bytes_per_row, &(x)->depth, NULL);\
} while (0)

struct picture_header2
{
    uint32_t z1;
    uint16_t bytes_per_row;
    uint16_t depth;
    uint32_t z2;
    uint32_t z3;
    uint32_t height;
    uint32_t width;
    uint32_t length;
};
#define FIX_PICTURE_HEADER2(x) FIX_PICTURE_HEADER(x)

struct bootloader_picture_footer
{
    uint32_t z1;                /* always 0 */
    uint16_t height;
    uint16_t width;
    uint16_t bytes_per_row;
    uint16_t depth;
    uint32_t z2;                /* always 0 */
    uint32_t length;
    uint16_t z3;                // some kind of offset, size of picture plus footer
    uint16_t z4;
};
#define FIX_PICTURE_FOOTER(x) do { \
    flip32(&(x)->z1, &(x)->z2, &(x)->length, NULL); \
    flip16(&(x)->height, &(x)->width, &(x)->bytes_per_row, &(x)->depth, \
           &(x)->z3, &(x)->z4, NULL);\
} while (0)

// ---------------------------------------------------------------------------------

struct font_table
{
    // these are relative to beginning of code (e.g. 0x4600)
    uint32_t cinfo_offset;
    uint32_t finfo_offset;
};

static const char FONT_MAGIC[] = { 0x04, 0x00, 0x21, 0x00 };

struct character_info
{
    char magic[4];              // FONT_MAGIC
    uint16_t first_char;
    uint16_t last_char;
    uint16_t z1;                // always 0x01
    uint16_t num_chars;
    uint32_t num_groups;
};
#define FIX_CHARACTER_INFO(x) do { \
    flip32(&(x)->num_groups, NULL); \
    flip16(&(x)->first_char, &(x)->last_char, &(x)->z1, &(x)->num_chars, NULL);\
} while (0)

struct unicode_group
{
    uint16_t start;
    uint16_t index;
    uint32_t length;
};
#define FIX_UNICODE_GROUP(x) do { \
    flip32(&(x)->length, NULL); \
    flip16(&(x)->start, &(x)->index, NULL); \
} while (0)


struct font_info
{
    uint16_t z1;                // always 0x04
    char font_name[66];
    uint32_t size;
    uint8_t style;              // bit 1 is set if bold
    uint8_t depth;              // one font Podium Sans is 4, everything else is 1
    uint16_t num_chars;         // always same as in struct character_info
    uint16_t fixed_width;       // either 0 or 1 depending on if characters have different widths
    uint16_t width;
    uint16_t line_height;       // line height
    uint16_t descent;           // pixels below baseline
    uint32_t t6;                // either 0 or 1. not sure what this is
    uint32_t finfo_size;        // always 0x64 (sizeof this struct?)
    uint32_t charmap_end; // from beginning of this struct to end of char map
    uint32_t metrics_end; // from beginning of this struct to end of char metrics
};
#define FIX_FONT_INFO(x) do { \
    flip32(&(x)->size, &(x)->t6, &(x)->finfo_size, &(x)->charmap_end, &(x)->metrics_end, NULL); \
    flip16(&(x)->z1, &(x)->num_chars, &(x)->fixed_width, &(x)->width, \
           &(x)->line_height, &(x)->descent, NULL);\
} while (0)

struct font_bitmap
{
    uint32_t z1;                // always 0
    uint16_t height;
    uint16_t width;
    uint16_t bytes_per_row;
    uint16_t depth;
    uint32_t z2;                // always 0
    uint32_t length;
};
#define FIX_FONT_BITMAP(x) do { \
    flip32(&(x)->z1, &(x)->z2, &(x)->length, NULL);\
    flip16(&(x)->height, &(x)->width, &(x)->bytes_per_row, &(x)->depth, NULL);\
} while (0)

// The following is apparently used when the font depth == 2
// I'm not currently using this yet
struct font_bitmap_depth2
{
    char z1[8];
    uint32_t height;
    uint32_t width;
    uint32_t bytes_per_row;
    char z2[12];
};

struct char_metric
{
    uint16_t left_offset;
    uint16_t right_offset;
    uint16_t width;
    int16_t left_indent;        // can be negative
};
#define FIX_CHAR_METRICS(x) do { \
    flip16(&(x)->left_offset, &(x)->right_offset, &(x)->width, &(x)->left_indent, NULL);\
} while (0)


// The following is apparently used when the font depth == 2
// I'm not currently using this yet
struct char_metric_depth2
{
    uint32_t left_offset;
    uint32_t right_offset;
    uint16_t width;
    int16_t left_indent;        // can be negative
};

struct font
{
    struct character_info cinfo;
    struct unicode_group *groups;
    struct font_info finfo;
    uint16_t *character_mapping;
    struct char_metric *metrics;
    struct font_bitmap bitmap;
};

// ---------------------------------------------------------------------------------

struct resource_block_header
{
    uint32_t z1;                // always 0x03
    uint32_t length;            // length to end of resource descriptions
    uint32_t num_sections;      // num of resource sections
};
#define FIX_RESOURCE_BLOCK_HEADER(x) do { flip32(&(x)->z1, &(x)->length, &(x)->num_sections, NULL); } while (0)

struct resource_section
{
    char type[4];               // four letters identifying type of resource e.g. ' rtS' 'paMB'
    uint32_t num_items;
    uint32_t t1;                // either 0 or 1
    uint32_t start;             // offset to beginning of block containing this resource type
};
#define FIX_RESOURCE_SECTION(x) do { flip32(&(x)->num_items, &(x)->t1, &(x)->start, NULL); } while (0)

struct resource
{
    int32_t id;
    uint32_t offset;            // from beginning of resource data at end of all sections
    uint32_t size;              // size of data (e.g. picture size including header)
};
#define FIX_RESOURCE(x) do { flip32(&(x)->id, &(x)->offset, &(x)->size, NULL); } while (0)

struct resource_block
{
    struct resource_block_header header;
    struct resource_section *sections;
    struct resource *resources;
};

// ---------------------------------------------------------------------------------

//#define IPOD_ADVANCED_USER

typedef void (*decode_resource) (FILE *in, char *out_filename);
typedef void (*encode_resource) (FILE *in, char *out_filename, void *arg);
typedef void (*info_resource) (FILE *out, FILE *in, struct resource *resource);

static void decode_text(FILE *in, char *file);
static void encode_text(FILE *in, char *file, void *arg);
static void info_text(FILE *out, FILE *in, struct resource *resource);
static void decode_bitmap(FILE *in, char *file);
static void encode_bitmap(FILE *in, char *file, void *arg);
static void info_bitmap(FILE *out, FILE *in, struct resource *resource);
static void encode_boot(FILE *in, char *file, void *arg);
static void decode_menu(FILE *in, char *file);
static void encode_menu(FILE *in, char *file, void *arg);
static void decode_type(FILE *in, char *file);
static void encode_type(FILE *in, char *file, void *arg);

#ifdef IPOD_ADVANCED_USER
static void decode_tcmd(FILE *in, char *file);
static void decode_view(FILE *in, char *file);
#endif

struct resource_type
{
    char type[5];               // internal type like 'paMB'
    char name[5];               // easier to read name like 'bmap'
    decode_resource decode;     // function to take raw resource to usable form
    encode_resource encode;     // go back to raw form
    info_resource info;         // print info
    char *extension;            // default extension for decoded resources
};

// don't know what all these actually mean yet, so some just reverse the type for the name
static struct resource_type RESOURCE_TYPES[] =
{
    {" rtS", "text", decode_text, encode_text, info_text, "txt"},
    {"paMB", "bmap", decode_bitmap, encode_bitmap, info_bitmap, "pgm"},
    {"uneM", "menu", decode_menu, encode_menu, NULL, "prop"}, /* menus */
    {"epyT", "type", decode_type, encode_type, NULL, "prop"}, /* font selector */

    // don't fully understand these files yet, this is just for my testing
#ifdef IPOD_ADVANCED_USER
    {"weiV", "view", decode_view, NULL, NULL, "prop"}, /* layout */
    {"dmCT", "tcmd", decode_tcmd, NULL, NULL, "prop"}, /* command table */

    {"enoN", "none", NULL, NULL, NULL, NULL}, /* space filler */
    {"tStS", "stst", NULL, NULL, NULL, NULL},

    // should not enable this, otherwise can get screwed up on
    // some firmwares finding incorrect resource block
#if 0
    // the following are found elsewhere in the firmware (not dumped from resource block)
    {"xaMV", "vmax", NULL, NULL, NULL, NULL},
    {"SgsM", "msgs", NULL, NULL, NULL, NULL},
    {"tuAk", "kaut", NULL, NULL, NULL, NULL},
    {"H&Pk", "kp&h", NULL, NULL, NULL, NULL},
    {" pUk", "kup ", NULL, NULL, NULL, NULL},
    {"emit", "time", NULL, NULL, NULL, NULL},
    {"dpUw", "wupd", NULL, NULL, NULL, NULL},
    {"ylpt", "tply", NULL, NULL, NULL, NULL},
    {"ltnC", "cntl", NULL, NULL, NULL, NULL},
    {"nwDk", "kdwn", NULL, NULL, NULL, NULL},
    {"leeW", "weel", NULL, NULL, NULL, NULL},
    {"GSlc", "clsg", NULL, NULL, NULL, NULL},
    {"GSlp", "plsg", NULL, NULL, NULL, NULL},
    {"GSon", "nosg", NULL, NULL, NULL, NULL},
#endif

#else
    {"weiV", "view", NULL, NULL, NULL, NULL},
    {"dmCT", "tcmd", NULL, NULL, NULL, NULL},
#endif
    {"mTDL", "ldtm", NULL, NULL, NULL, NULL}, /* language something */
    {"stiB", "bits", NULL, NULL, NULL, NULL},

    // these are pseudo resources. These were just chosen to not match existing ones
    {"boot", "boot", NULL, encode_boot, NULL, "pgm"}, /* for pictures in the bootloader */
};

static char *FILE_EXTENSIONS[] = { "rsrc", "pgm", "ppm", "txt", "prop", };

static struct resource_type unknown_resource_type = { "????", "????", NULL, NULL, NULL, NULL };

// ---------------------------------------------------------------------------------

typedef enum property_type
{
    ID,
    TYPE,
    DECIMAL,
    HEX,
} property_type;

#define MAX_PROPERTY_LENGTH 16
struct property
{
    property_type type;
    char name[MAX_PROPERTY_LENGTH];
    int data;
    char *comment;
};

#define MAX_PROPERTIES 32
struct properties
{
    int num_props;
    struct property props[MAX_PROPERTIES];
};

// list of elements, each has a set of properties
#define MAX_ELEMENTS 90
struct property_list
{
    int num_elems;
    struct properties elems[MAX_ELEMENTS];
};

struct property_field
{
    property_type type;
    char *name;
    int size;                   // number of double-bytes, 1 or 2
    char *help;
};

// Description of the other resources to be read into property files
static struct property_field resource_type_type[] =
{
    { ID, "fontID", 1, "ID of font" },    /* rsrc ID of text containing font name */
    { HEX, "z1",  1, NULL},
    { DECIMAL, "size",  2, "Font size"},
    { HEX, "z2",  2, NULL},
    { HEX, "z3",  2, NULL},           /* must be font description of some sort */
};

static struct property_field resource_type_menu[] =
{
    { DECIMAL, "size", 2, "Size of this struct. Always 36." },
    { HEX, "z1", 2, NULL },           /* 32,34,36,38 */
    { HEX, "z2", 2, NULL }, /* 7434,7438,9218,9342,9398. quite different across firmwares */
    { HEX, "cmd", 2, NULL }, /* varied numbers around 7000-9000. maybe command to execute on press. byte offset into a table? they are all separated by dwords */
    { ID, "submenuID", 1, "ID of submenu" },
    { HEX, "z3", 1, "Zero" },
    { ID, "textID", 1, "text displayed by menu" },
    { HEX, "z4", 1, NULL }, /* numbers increase from 775 within one menu. same across firmwares */
    { HEX, "z5", 2, NULL },           /* mostly unique numbers */
    { HEX, "z6", 2, NULL }, /* similar numbers across very different menus */
    { ID, "textID2", 1, "Always equal to textID? unknown function" },
    { HEX, "z7", 1, "Zero" },
    { ID, "typeID", 2, "Zero indicates to use default type (font)" },
};


struct resource_view_temp
{
    uint32_t a1;
    uint16_t a2;
    uint16_t a3;
    int32_t a4;                 /* width, height */
};
#define FIX_RESOURCE_TYPE_VIEW_TEMP(x) do { flip32(&(x)->a1, &(x)->a4, NULL); \
    flip16(&(x)->a2, &(x)->a3, NULL);\
} while (0)

struct resource_type_view_elem
{
    uint32_t size;              /* size of rest of resource */
    uint32_t z0;
    char type[4];               /* resource type */
    int32_t id;                 /* resource ID */
    int32_t z1;
    int32_t z2;

    // appear to be bit fields
    uint16_t z3;
    uint16_t z4;

    // 1.a4 for text is length
    // 2.a4 is height 3.a4 is width for bitmap
    struct resource_view_temp tmp[4];

    // some number of extra values
};
#define FIX_RESOURCE_TYPE_VIEW(x) do { flip32(&(x)->size, &(x)->z0, &(x)->id, &(x)->z1, &(x)->z2, NULL); \
    flip16(&(x)->z3, &(x)->z4, NULL);\
} while (0)

struct resource_type_view
{
    int32_t num_elem;
    struct resource_type_view_elem *elements;
};

struct resource_type_tcmd
{
    uint32_t size_unk;
    uint16_t z1;                /* zero */
    uint16_t num_elems;
    int32_t *elems;             /* num_elems elements */
    uint16_t size2;             /* number of uint32_t after z2 */
    uint16_t t2;                /* always 2 */
    uint16_t t3;                /* some kind of length */
    uint16_t t4;                /* some kind of length */
    uint32_t z2;                /* zero */
    uint32_t *nums;             /* offsets? size2 of them. first one always 0x4E (78) */
    uint16_t size3;
    uint16_t z3;                /* always 1 */
    uint32_t *addrs;            /* not addresses, what is this? */
};

// ---------------------------------------------------------------------------------
// Globals

static int big_endian = 0; // whether we are on a big-endian machine
static const char *program_name = "ipodedit"; // name of this program

// ---------------------------------------------------------------------------------

#define MAX_FILE_SEARCH (100 * 1024 * 1024) /* max number of bytes for random searching */
#define MAX_FILENAME (PATH_MAX+1)
#define BUFFER_SIZE 1024
#define NUM_ELEM(x) ((int)(sizeof(x) / sizeof(x[0])))
#define DWORD_ALIGN(x) ((((x)+3)/4)*4)
#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a,b) ((a) > (b) ? (b) : (a))
#endif
#define UNUSED_VAR(a) ((void)a) // to avoid unused variable warnings

// ---------------------------------------------------------------------------------

// info from http://www.ipodlinux.org/index.php/Firmware

#define FIRMWARE_VERSION_ADDR 0x10a
#define FIRMWARE_HEADER_ADDR 0x4200
#define FIRMWARE_MAX_IMAGES 12
#define IPODLINUX_BOOT_TABLE_OFFSET 0x100  // from startup.s and make_fw.c

// this is always at beginning of the firmware
// must use sizeof for following instead of strlen as there is an embedded NULL
// must use char [] instead of char * to have sizeof work
static const char FIRMWARE_HEADER_MAGIC[] = "{{~~  /-----\\   {{~~ /       \\  {{~~|         | {{~~| S T O P | {{~~|         | {{~~ \\       /  {{~~  \\-----/   Copyright(C) 2001 Apple Computer, Inc.---------------------------------------------------------------------------------------------------------\0]ih[";
static const char FIRMWARE_IMAGE_MAGIC[] = "!ATA";
static const char FIRMWARE_OS_ID[] = "soso"; /* OS */
static const char FIRMWARE_RSRC_ID[] = "crsr"; // in newest firmware, apparently resource image
static const char FIRMWARE_SOFTUPDT_ID[] = "dpua"; /* apple update */

// location of the portalplayer text in the apple OS image is within
// first FIRMWARE_PORTALPLAYER_LOCATION bytes
#define FIRMWARE_PORTALPLAYER_LOCATION 64
static const char FIRMWARE_PORTALPLAYER[] = "portalplayer";

struct firmware_image_info
{
    char magic[4];              /* '!ATA' */
    char id[4];                 /* 'soso' for retailos, 'dpua' for bootloader/softupdt */
    uint32_t flashed;           /* normally zero, seems to be set to 1 after flashing */
    uint32_t code_offset;       /* byte offset of start of image code */
    uint32_t length;            /* length in bytes of image */
    uint32_t addr;              /* load address */
    uint32_t entry_offset;      /* execution start within image */
    uint32_t checksum;          /* checksum for image */
    uint32_t version;           /* image version */
    uint32_t load_addr;         /* load address for image */
};
#define FIX_FIRMWARE_IMAGE_INFO(x) do { flip32(&(x)->code_offset, &(x)->length, \
                                               &(x)->addr, &(x)->entry_offset, &(x)->checksum, \
                                               &(x)->version, &(x)->load_addr, NULL); \
} while (0)

#define FOURTH_GEN(fw) (((fw)->version == 3) ? 1 : 0)
typedef struct firmware
{
    char *filename;
    FILE *file;                 /* firmware file */

    // We read in the firmware completely into memory to speed up
    // some I/O operations, especially when running on the iPod.
    // To guarantee that the following is identical to what is in file,
    // we make it const and anytime we write to the firmware file, we
    // also update this so it is a consistent view.
    // pos is a kind of pseudo-file position marker representing
    // the current position reading/writing in data.
    const unsigned char *data;                 /* entire firmware file */
    long size;                  /* total size of the data */
    long pos;

    // version will be 3 for 4th gen and 2 for all others
    unsigned char version; // byte at FIRMWARE_VERSION_ADDR
    unsigned int num_images;
    struct firmware_image_info images[FIRMWARE_MAX_IMAGES];
    int image_addr[FIRMWARE_MAX_IMAGES]; // location of image header, needed for ipodlinux
    struct firmware_image_info *retailos;
    struct firmware_image_info *softupdt;
    struct firmware_image_info *ipodlinux_master_image; // master image generated by iPodLinux

    FILE *resource_file;        /* file to print resource info */
} firmware_t;

// ---------------------------------------------------------------------------------

typedef void (*command_main) (int argc, char **argv);
struct command_map
{
    char *name;
    char *shortname;
    command_main main;
    int min_args;
};

// ---------------------------------------------------------------------------------

// For endian conversion
// The iPod is little-endian
// correct integers on big-endian machines
static void check_endian()
{
    unsigned int test = 1;
    if ( * (unsigned char *) &test == 0 )
        big_endian = 1;
}

static inline uint16_t _flip16(uint16_t x)
{
    return (((x >> 8) & 0x00ff) | ((x << 8) & 0xff00));
}

static void flip16(uint16_t *args, ...)
{
    // flips a list of uint16s. The list must be terminated by NULL
    va_list ap;

    if (! big_endian)
        return;

    va_start(ap, args);
    while (args != 0)
    {
        *args = _flip16(*args);
        args = va_arg(ap, uint16_t *);
    }
    va_end(ap);
}

static inline uint32_t _flip32(uint32_t x)
{
    return (((x >> 24) & 0x000000ff) | ((x >> 8) & 0x0000ff00) |
            ((x << 8) & 0x00ff0000) | ((x << 24) & 0xff000000));
}

static void flip32(uint32_t *args, ...)
{
    // flips a list of uint32s. The list must be terminated by NULL
    va_list ap;

    if (! big_endian)
        return;

    va_start(ap, args);
    while (args != 0)
    {
        *args = _flip32(*args);
        args = va_arg(ap, uint32_t *);
    }
    va_end(ap);
}

// ---------------------------------------------------------------------------------

static void perror_exit(char *str)
{
    perror(str);
    exit(1);
}

static void error(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    printf("Error: ");
    vprintf(format, ap);
    printf("\n");
    va_end(ap);
    exit(1);
}

static void warning(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    printf("Warning: ");
    vprintf(format, ap);
    printf("\n");
    va_end(ap);
}

static void info(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vprintf(format, ap);
    printf("\n");
    va_end(ap);
}

static void debug(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
#if DEBUG
    vprintf(format, ap);
    printf("\n");
#endif
    va_end(ap);
}

static int usage()
{
    printf("Usage: %s <cmd> [arguments]\n", program_name);
    printf("All commands have a short and long form.\n");
    printf("Valid commands:\n\n");

    printf("[extract|x] <firmware> [num]\n");
    printf("\tExtracts resources from the firmware.\n");
    printf("\tIf num is > 0, then extracts only that language resource block number.\n");
    printf("\tIf num is 0, extracts only the primary resource block.\n");
    printf("\tIf num is -1, extracts all languages and the primary resource block.\n");
    printf("\tIf num is omitted, it defaults to 0 (only the primary resource block).\n\n");

    printf("[extract|x] <updater> [num]\n");
    printf("\tExtracts firmwares from an iPod firmware updater (or any other file).\n");
    printf("\tIf num is 0, extracts all firmwares.\n");
    printf("\tIf num is > 1, extracts first n firmwares.\n");
    printf("\tIf num is omitted, it defaults to 0 (all firmwares).\n\n");

    printf("[prepare|p] <file> [depth]\n");
    printf("\tEncodes the file into a raw resource file.\n");
    printf("\tFor grayscale bitmap files, the target depth is required.\n");
    printf("\tColor bitmaps and other resources do not need to specify a depth.\n\n");

    printf("[decode|d] <file.rsrc> ...\n");
    printf("\tDecodes raw resource files into a more easily editable form.\n");
    printf("\tThe program automatically decodes resources during extraction.\n\n");

    printf("[map|m] <directory> [mapfile]\n");
    printf("\tTakes resource file names specific to a particular firmware and map\n");
    printf("\tthem to generic names using mapfile.\n");
    printf("\tAll new files will be put into the directory %s.\n", MAP_DIR);
    printf("\tIf mapfile is not specified, the default '%s' is used.\n\n", RESOURCE_MAP_FILE);

    printf("[unmap|u] <directory> [mapfile]\n");
    printf("\tPerforms filename mapping in reverse direction of the map command.\n");
    printf("\tAll new files will be put into the the directory %s.\n", DATA_DIR);
    printf("\tIf mapfile is not specified, the default '%s' is used.\n\n", RESOURCE_MAP_FILE);

    printf("[write|w] <firmware> <directory> [num]\n");
    printf("\tWrites resource files in the directory to the firmware.\n");
    printf("\tNum has the same meaning as for the extract command.\n");
    printf("\tIf num is omitted, it defaults to -1 (all blocks are written).\n\n");

    printf("[write|w] <updater> <directory> [num]\n");
    printf("\tWrites firmware files in the directory back to the updater file.\n");
    printf("\tNum has the same meaning as for the extract command.\n");
    printf("\tIf num is omitted, it defaults to 0 (all firmwares are written).\n\n");

    printf("[info|i] <firmware>\n");
    printf("\tPrints information about the code images located in the firmware.\n\n");

    printf("[checksum|c] <firmware>\n");
    printf("\tCalculates and updates the checksum for all images in the firmware.\n\n");

    /*
    // XXX: can this be handled more generically like the rest of the resources?
    printf("[fonts|f] <firmware>\n");
    printf("\tDumps all font bitmaps from the firmware.\n\n");
    */

    return 0;
}

// ---------------------------------------------------------------------------------

static inline void goto_byte(FILE *stream, unsigned int offset)
{
    // wrapper around fseek
    if (fseek(stream, offset, SEEK_SET) == -1)
        perror_exit("fseek");
}

static inline void _read_bytes(FILE *stream, void *buf, size_t size, int error_on_eof)
{
    // wrapper around fread. read size bytes and put them into buf
    if (fread(buf, 1, size, stream) != size)
    {
        if (feof(stream))
        {
            if (error_on_eof)
                error("Unexpected end-of-file while reading");
        }
        else
            perror_exit("fread");
    }
}

static inline void read_bytes(FILE *stream, void *buf, size_t size)
{
    // by default, we have unrecoverable error if we hit eof
    _read_bytes(stream, buf, size, 1);
}

static inline void write_bytes(FILE *stream, const void *buf, size_t size)
{
    // wrapper around fwrite. write size bytes from buf
    if (fwrite(buf, 1, size, stream) != size)
    {
        if (feof(stream))
        {
            // is this even possible?
            error("Unexpected end-of-file while writing!");
        }
        else
            perror_exit("fwrite");
    }
}

static inline void _read_bytes_from(FILE *stream, unsigned int addr,
                                    void *buf, size_t size, int error_on_eof)
{
    goto_byte(stream, addr);
    _read_bytes(stream, buf, size, error_on_eof);
}

static inline void read_bytes_from(FILE *stream, unsigned int addr,
                                   void *buf, size_t size)
{
    _read_bytes_from(stream, addr, buf, size, 1);
}

static inline void write_bytes_to(FILE *stream, unsigned int addr,
                                  const void *buf, size_t size)
{
    goto_byte(stream, addr);
    write_bytes(stream, buf, size);
}

static inline void copy_bytes(FILE *dest, FILE *source, int begin, unsigned int num_bytes)
{
    // copy num_bytes from the begin byte in source directly into dest
    char buf[BUFFER_SIZE];
    unsigned int num;

    goto_byte(source, begin);
    while (num_bytes > 0)
    {
        num = MIN(num_bytes, sizeof(buf));
        read_bytes(source, buf, num);
        write_bytes(dest, buf, num);
        num_bytes -= num;
    }
}

static inline void check_isdir(char *dir)
{
    struct stat dirstat;
    if (stat(dir, &dirstat) || ! S_ISDIR(dirstat.st_mode))
        error("%s is not a directory", dir);
}

static inline long fsize(FILE *stream)
{
    // return the size of the file
    long size;
    if (fseek(stream, 0, SEEK_END) == -1)
        perror_exit("fseek");
    if ((size = ftell(stream)) == -1)
        perror_exit("ftell");
    return size;
}

static void insert_file(FILE *dest, char *source, int begin, int num_bytes)
{
    // opens up the file given by source
    // inserts the entire source file if it exists into dest starting from begin with num_bytes
    FILE *in;
    if ((in = fopen(source, "r")))
    {
        info("\t => Found %s to write", source);
        // check it's the right size before write
        if (fsize(in) != num_bytes)
        {
            warning("File size (%ld) is not expected size (%d). Not writing.",
                    fsize(in), num_bytes);
        }
        else
        {
            goto_byte(dest, begin);
            copy_bytes(dest, in, 0, num_bytes);
        }
        fclose(in);
    }
    warning("File %s doesn't exist, skipping...", source);
}

static inline int at_eof(FILE *file)
{
    // checks we're at the end of the given file
    int pos = ftell(file);
    int end;
    fseek(file, 0, SEEK_END);
    end = ftell(file);
    goto_byte(file, pos);       // restore original position
    return (pos == end);
}

// ---------------------------------------------------------------------------------

/*
static int property_exists(struct properties *props, char *name)
{
    int i;
    for (i = 0; i < props->num_props; i++)
    {
        if (strcmp(name, props->props[i].name) == 0)
            return 1;
    }
    return 0;
}
*/

static int get_property_value(struct properties *props, char *name)
{
    int i;
    for (i = 0; i < props->num_props; i++)
    {
        if (strcmp(name, props->props[i].name) == 0)
            return props->props[i].data;
    }
    error("Could not find value for property '%s'", name);
    return 0;                   /* unreachable */
}

static void add_property(struct properties *props, char *name,
                         property_type type, int data, char *comment)
{
    struct property *prop;
    if (props->num_props >= MAX_PROPERTIES)
        error("Number of properties > MAX_PROPERTIES");

    prop = &props->props[props->num_props];
    prop->type = type;
    strncpy(prop->name, name, sizeof(prop->name));
    prop->name[sizeof(prop->name)-1] = '\0'; /* make sure string is null terminated */
    prop->data = data;
    prop->comment = comment;
    props->num_props++;
}

static void read_property_fields(FILE *in, struct properties *props,
                                 struct property_field fields[], int num_fields)
{
    int i, data;
    int16_t i16;
    int32_t i32;
    struct property_field *field;
    for (i = 0; i < num_fields; i++)
    {
        field = &fields[i];
        if (field->size == 1)
        {
            read_bytes(in, &i16, sizeof(i16));
            flip16(&i16, NULL);
            data = i16;
        }
        else
        {
            read_bytes(in, &i32, sizeof(i32));
            flip32(&i32, NULL);
            data = i32;
        }
        add_property(props, field->name, field->type, data, field->help);
    }
}

static void write_property_fields(FILE *out, struct properties *props,
                                  struct property_field fields[], int num_fields)
{
    int i, data;
    int16_t i16;
    int32_t i32;
    struct property_field *field;
    for (i = 0; i < num_fields; i++)
    {
        field = &fields[i];
        data = get_property_value(props, field->name);
        if (field->size == 1)
        {
            i16 = data;
            flip16(&i16, NULL);
            write_bytes(out, &i16, sizeof(i16));
        }
        else
        {
            i32 = data;
            flip32(&i32, NULL);
            write_bytes(out, &i32, sizeof(i32));
        }
    }
}

static void read_property_file(FILE *in, struct property_list *list)
{
    char line[BUFFER_SIZE];
    int lineno = 1;
    struct properties *props = NULL;
    struct property *prop;
    char ch;

    memset(list, 0, sizeof(*list));

    if ( ! fgets(line, sizeof(line), in))
        perror_exit("fgets");

    while (list->num_elems < MAX_ELEMENTS)
    {
        if ( ! fgets(line, sizeof(line), in))
        {
            if (feof(in))
            {
                if (props != NULL)
                    error("No closing <end> at end of file");
                return;
            }
            perror_exit("fgets");
        }
        lineno++;

        // skip empty lines
        if (sscanf(line, " %c", &ch) != 1)
            continue;           // line has no non-whitespace character

        // skip comments
        // comment char must be first character
        if (line[0] == '#')
            continue;

        if (line[0] == '<')
        {
            if (strncmp(line, "<begin>", strlen("<begin>")) == 0)
            {
                // start a new element
                if (props != NULL)
                    error("No <end> found on line %d", lineno);
                props = &list->elems[list->num_elems];
                continue;
            }
            else if (strncmp(line, "<end>", strlen("<end>")) == 0)
            {
                // end an element
                if (props == NULL)
                    error("No <begin> for <end> line %d", lineno);
                list->num_elems++;
                props = NULL;
                continue;
            }
        }

        if ((props == NULL) || (props->num_props >= MAX_PROPERTIES))
        {
            warning("Ignoring line %d: \n  %s", lineno, line);
            continue;
        }

        prop = &props->props[props->num_props];
        if (sscanf(line, "%s = ", prop->name) != 1)
            error("Reading property file on line %d:\n  %s", lineno, line);
        
        prop->type = DECIMAL;
        if ((sscanf(line, "%*s = 0x%x", &prop->data) != 1) &&
            (sscanf(line, "%*s = %d", &prop->data) != 1))
        {
            error("Reading property file on line %d:\n  %s", lineno, line);
        }
        props->num_props++;
    }
    error("The number of elements is greater than %d", MAX_ELEMENTS);
}

static void write_property_file(char *file, struct property_list *list)
{
    FILE *out;
    int i, j;
    struct properties *props;
    struct property *prop;
    unsigned char filename[MAX_FILENAME];
    FILE *text;

    if (! (out = fopen(file, "w")))
        perror_exit("fopen");

    fprintf(out, "## %s\n", basename(file));
    for (i = 0; i < list->num_elems; i++)
    {
        fprintf(out, "\n# Element %d\n<begin>\n", i);
        props = &list->elems[i];
        for (j = 0; j < props->num_props; j++)
        {
            prop = &props->props[j];
            fprintf(out, "    %-10s = ", prop->name);
            switch (prop->type)
            {
                case ID:
                    fprintf(out, "%-10d  ##", prop->data);

                    // if text file of resource exists, grab the text
                    // seems easier with this inefficiency than keeping all strings in memory
                    snprintf(filename, sizeof(filename), "%s/text%d.txt", DATA_DIR, prop->data);
                    if ((text = fopen(filename, "r")))
                    {
                        fprintf(out, " (");
                        copy_bytes(out, text, 0, fsize(text));
                        fprintf(out, ")");
                        fclose(text);
                    }
                    break;
                    
                case TYPE:
                    fprintf(out, "%-10.4s  ##", (char * )&prop->data);
                    break;

                case DECIMAL:
                    fprintf(out, "%-10d  ##", prop->data);
                    break;

                case HEX:
                    fprintf(out, "0x%-8x  ## (%d)", prop->data, prop->data);
                    break;

                default:
                    error("Unknown property type: %d", prop->type);
            }
            if (prop->comment)
                fprintf(out, " %s", prop->comment);
            fprintf(out, "\n");
        }

        fprintf(out, "<end>\n");
    }
    fclose(out);
}

// ---------------------------------------------------------------------------------

static void print_firmware_image_info(struct firmware_image_info *fw_info)
{
    info("Image ID: %.4s", fw_info->id);
    info("Flashed: 0x%x", fw_info->flashed);
    info("Code Begin: 0x%x", fw_info->code_offset);
    info("Code End: 0x%x", fw_info->code_offset+fw_info->length);
    info("Code Length: 0x%x", fw_info->length);
    info("Address: 0x%x", fw_info->addr);
    info("Entry Offset: 0x%x", fw_info->entry_offset);
    info("Image Version: 0x%x", fw_info->version);
    info("Image Load Address: 0x%x", fw_info->load_addr);
    info("Existing Checksum: 0x%x", fw_info->checksum);
}

static void close_firmware(firmware_t *fw)
{
    fclose(fw->file);
    if (fw->resource_file)
        fclose(fw->resource_file);
    if (fw->data)
        free((unsigned char *)fw->data);
    free(fw);
}

static inline void fw_seek(firmware_t *fw, long newpos)
{
    fw->pos = newpos;
}

static inline long fw_tell(firmware_t *fw)
{
    return fw->pos;
}

static inline void fw_read(firmware_t *fw, void *buf, size_t size)
{
    // reads from fw->pos size bytes into buf
    if ((fw->pos + (long)size > fw->size) || (fw->pos < (long)0))
        error("Cannot read 0x%x bytes from pos 0x%x in file (length=0x%x)", size, fw->pos, fw->size);
    memcpy(buf, fw->data + fw->pos, size);
    fw->pos += size;
}

static inline void fw_read_from(firmware_t *fw, unsigned int addr,
                                void *buf, size_t size)
{
    // reads bytes from a specified position in the firmware into buf
    if ((int)addr + (int)size > fw->size)
        error("Cannot read 0x%x bytes from pos 0x%x in file (length=0x%x)", size, addr, fw->size);
    memcpy(buf, fw->data + addr, size);
    fw->pos = addr + size;
}

static inline void fw_write_to(firmware_t *fw, unsigned int addr,
                        const void *buf, size_t size)
{
    // write new data to firmware file
    // also update fw->data to match file on disk for consistency
    write_bytes_to(fw->file, addr, buf, size);
    memcpy((unsigned char *)fw->data + addr, buf, size);
}

static inline void fw_write_from_file(firmware_t *fw, unsigned int addr,
                                      FILE *source, size_t size)
{
    // write new data to firmware file from beginning of source FILE
    char *buf = malloc(size);
    goto_byte(source, 0);
    read_bytes(source, buf, size);
    fw_write_to(fw, addr, buf, size);
    free(buf);
}

static int is_firmware(FILE *file, int begin)
{
    // checks if an ipod firmware appears to start from location begin in the file
    char buf[sizeof(FIRMWARE_HEADER_MAGIC)];
    struct firmware_image_info header;

    _read_bytes_from(file, begin, buf, sizeof(buf), 0);
    if (feof(file))
        return 0;
    if (memcmp(buf, FIRMWARE_HEADER_MAGIC, sizeof(FIRMWARE_HEADER_MAGIC)) != 0)
        return 0;

    // also try reading the firmware image header
    _read_bytes_from(file, begin+FIRMWARE_HEADER_ADDR, &header, sizeof(header), 0);
    if (feof(file))
        return 0;
    FIX_FIRMWARE_IMAGE_INFO(&header);
    if (memcmp(header.magic, FIRMWARE_IMAGE_MAGIC, strlen(FIRMWARE_IMAGE_MAGIC)) != 0)
        return 0;

    // pretty sure it is a firmware if reach here
    return 1;
}

static int is_retailos(firmware_t *fw, struct firmware_image_info *image)
{
    // tries to determine if the given image is likely to be an apple OS image
    // isn't sufficient check that image ID is 'soso' because linux also uses that
    // we look for text near the beginning of the code identifying it as portalplayer
    char buf[strlen(FIRMWARE_PORTALPLAYER)+1];
    int i;
    for (i = 0; i < FIRMWARE_PORTALPLAYER_LOCATION; i += 4)
    {
        goto_byte(fw->file, image->code_offset + image->entry_offset + i);
        read_bytes(fw->file, buf, sizeof(buf));
        if (memcmp(buf, FIRMWARE_PORTALPLAYER, strlen(FIRMWARE_PORTALPLAYER)) == 0)
            return 1;
    }
    return 0;
}

static int firmware_size(firmware_t *fw)
{
    unsigned int i, length = 0;
    for (i = 0; i < fw->num_images; i++)
        length = MAX(length, fw->images[i].code_offset+fw->images[i].length);
    return length;
}

static void read_firmware_images(firmware_t *fw, int offset)
{
    struct firmware_image_info *image;

    goto_byte(fw->file, offset);
    while (fw->num_images < FIRMWARE_MAX_IMAGES)
    {
        image = &fw->images[fw->num_images];
        fw->image_addr[fw->num_images] = ftell(fw->file);
        read_bytes(fw->file, image, sizeof(struct firmware_image_info));
        FIX_FIRMWARE_IMAGE_INFO(image);
        if (memcmp(image->magic, FIRMWARE_IMAGE_MAGIC, strlen(FIRMWARE_IMAGE_MAGIC)) != 0)
            return;
        fw->num_images++;
    }
}

static void read_firmware_info(firmware_t *fw, int start)
{
    // reads from FILE fw->file starting from position start
    // and fills in the firmware struct with the image data

    unsigned int i;
    struct firmware_image_info *image;

    // figure out whether we have 1,2,3 or 4th gen firmware
    read_bytes_from(fw->file, start + FIRMWARE_VERSION_ADDR, &fw->version, 1);
    fw->num_images = 0;
    read_firmware_images(fw, start + FIRMWARE_HEADER_ADDR);
        
    if (fw->num_images == 0)
        error("No images found in firmware!");

    // extra 512 bytes in front of osos images for some reason on 4th gen
    if (FOURTH_GEN(fw))
    {
        for (i = 0; i < fw->num_images; i++)
        {
            image = &fw->images[i];
            if (memcmp(image->id, FIRMWARE_OS_ID, strlen(FIRMWARE_OS_ID)) == 0)
                image->code_offset += 512;
        }
    }

    if (fw->num_images == 1)
    {
        // ipodlinux generates a master image that includes everything
        // inside can in theory contain multiple images of either Apple or Linux types
        // The master image entry offset points to beginning of loader code
        // The images that can be loaded by the loader start 0x100 bytes after that
        // this number is hard-coded in make_fw.c and startup.s
        read_firmware_images(fw, fw->images[0].code_offset +
                             fw->images[0].entry_offset + IPODLINUX_BOOT_TABLE_OFFSET);
        if (fw->num_images > 1)
        {
            info("Appears to be an iPodLinux generated firmware. Found %d images.", fw->num_images-1);
            fw->ipodlinux_master_image = &fw->images[0];
        }
    }

    for (i = 0; i < fw->num_images; i++)
    {
        image = &fw->images[i];
        if (image == fw->ipodlinux_master_image)
            continue;
        else if (memcmp(image->id, FIRMWARE_SOFTUPDT_ID, strlen(FIRMWARE_SOFTUPDT_ID)) == 0)
            fw->softupdt = image;
        else if (is_retailos(fw, image))
        {
            if (fw->retailos)
                warning("Multiple Apple OS firmware images found. Only using the first one.");
            else
                fw->retailos = image;
        }
    }
}

static firmware_t *open_firmware(char *name, const char *mode, int error_if_not_firmware)
{
    // opens a file as an iPod firmware and checks that it looks okay
    // if error_if_not_firmware is non-zero, then it is a fatal error for a non-firmware file
    // if it is zero, then NULL is returned for non-firmware files
    firmware_t *fw;
    FILE *firm;

    if (! (firm = fopen(name, mode)))
        error("Cannot open file: %s", name);

    if (! is_firmware(firm, 0))
    {
        warning("%s does not look like an iPod firmware", name);
        fclose(firm);
        if (error_if_not_firmware)
            exit(1);
        else
            return NULL;
    }

    fw = malloc(sizeof(*fw));
    memset(fw, 0, sizeof(*fw));

    fw->filename = name;
    fw->file = firm;
    read_firmware_info(fw, 0);

    if ((fw->num_images == 0) || (fw->retailos == NULL))
    {
        close_firmware(fw);
        if (error_if_not_firmware)
            error("No OS image found in %s", name);
        else
            warning("No OS image found in %s", name);
        return NULL;
    }

    fw->size = firmware_size(fw);
    fw->data = malloc(fw->size);
    goto_byte(fw->file, 0);
    read_bytes(fw->file, (unsigned char *)fw->data, fw->size);

    return fw;
}

static void update_checksum(firmware_t *fw, int write)
{
    // calculates checksum for all firmware images
    // if write == 0, calculate and print new checksum but don't write it
    struct firmware_image_info *image;
    int i;
    unsigned int j, sum;

    info("\niPod Firmware Generation: %s", FOURTH_GEN(fw) ? "4th" : "1st-3rd");
    info("Found %d images", fw->num_images);

    // we update the checksums of the firmware images in reverse order because
    // for ipodlinux firmware, image 0 is the master image that includes the other
    // images. So the checksum for other images need to be updated before master image
    // for non-ipodlinux firmware, it doesn't matter.
    for (i = fw->num_images-1; i >= 0; i--)
    {
        image = &fw->images[i];
        info("\n=> Image %d at 0x%x %s", i, fw->image_addr[i],
             image == fw->softupdt ? "(softupdt)" : image == fw->retailos ? "(retailos)" :
             image == fw->ipodlinux_master_image ? "(ipodlinux master)" : "(linux?)");
        print_firmware_image_info(image);

        // calculate new checksum
        sum = 0;
        for (j = 0; j < image->length; j++)
        {
            sum += fw->data[image->code_offset+j];
        }
        info("Calculated Checksum: 0x%x", sum);

        // 4th gen softupdts are encrypted so never try to update
        if (FOURTH_GEN(fw) && (image == fw->softupdt))
        {
            info("Checksum mismatch expected for 4th generation bootloader. Ignoring...");
            continue;
        }

        // the rsrc block checksum isn't calculated right yet
        if (memcmp(image->id, FIRMWARE_RSRC_ID, strlen(FIRMWARE_RSRC_ID)) == 0)
        {
            info("Skipping rsrc image for now, expected checksum mismatch...");
            continue;
        }

        if (write)
        {
            if (sum != image->checksum)
            {
                flip32(&sum, NULL);
                fw_write_to(fw,
                            fw->image_addr[i] + (unsigned int)&image->checksum - (unsigned int)image,
                            &sum, sizeof(sum));
                info("Wrote new checksum.");
            }
            else
                info("Checksum unchanged. Not writing.");
        }
        else
        {
            if (sum != image->checksum)
                warning("checksums do not match! Use checksum command to write.");
        }
    }
}

// ---------------------------------------------------------------------------------

static int known_resource_type(const char *type)
{
    int i;
    for (i = 0; i < NUM_ELEM(RESOURCE_TYPES); i++)
    {
        if (memcmp(RESOURCE_TYPES[i].type, type, 4) == 0)
            return 1;
    }
    return 0;
}

static struct resource_type *get_resource_type(char *type)
{
    int i;
    for (i = 0; i < NUM_ELEM(RESOURCE_TYPES); i++)
    {
        if ((memcmp(RESOURCE_TYPES[i].type, type, 4) == 0) ||
            (memcmp(RESOURCE_TYPES[i].name, type, 4) == 0))
            return &RESOURCE_TYPES[i];
    }
    warning("Unknown type '%.4s'", type);
    return &unknown_resource_type;
}

static struct resource_type *parse_resource_filename(char *name, int *block, int *id)
{
    // parse our format for resources to get the resource info
    char type[5];

    name = basename(name); // get the filename only
    *block = 0;
    *id = 0;
    if ((sscanf(name, "%d-%4s%d.", block, type, id) != 3) &&
        (sscanf(name, "%4s%d.", type, id) != 2))
    {
        error("%s is not a valid filename. You must use the names dumped by the program.", name);
    }
    return get_resource_type(type);
}

static void info_text(FILE *out, FILE *in, struct resource *resource)
{
    // prints out the string length
    // does not include NULL character in string length
    fprintf(out, " length=%d", resource->size-1);
    UNUSED_VAR(in);
}

static void decode_text(FILE *in, char *out_filename)
{
    // for decoding text, simply remove the trailing NULL byte
    FILE *out;
    char null;
    int len = fsize(in);
    if (! (out = fopen(out_filename, "w")))
        perror_exit("fopen");
    copy_bytes(out, in, 0, len-1);
    read_bytes(in, &null, sizeof(null)); // read the last byte for completeness
    fclose(out);
}

static void encode_text(FILE *in, char *file, void *arg)
{
    int len = fsize(in);
    FILE *out;
    char *text, *ptr;

    if (! (out = fopen(file, "w")))
        perror_exit("fopen");

    text = malloc(len+1);
    read_bytes_from(in, 0, text, len);

    // remove trailing newlines/carriage returns
    for (ptr = text + len - 1; *ptr == '\r' || *ptr == '\n'; ptr--)
        ;
    *(++ptr) = '\0'; // add null terminator
    write_bytes_to(out, 0, text, ptr-text+1);

    free(text);
    fclose(out);
    UNUSED_VAR(arg);
}

static int valid_bitmap(struct bitmap_info *bitmap)
{
    if (! bitmap->height || ! bitmap->width || ! bitmap->length || ! bitmap->bytes_per_row)
        return 0;
    if (bitmap->bytes_per_row != ((int)(((bitmap->width*bitmap->depth+7)/8+3)/4))*4)
        return 0;
    if (bitmap->length != bitmap->height * bitmap->bytes_per_row)
        return 0;
    return 1;
}

static void dump_bitmap_info(struct bitmap_info *bitmap)
{
    info("\theight = %d\n\twidth = %d\n\tbytes/line = %d\n\tdepth = %d\n\tsize = %d",
         bitmap->height, bitmap->width, bitmap->bytes_per_row, bitmap->depth, bitmap->length);
}

static void read_bitmap(FILE *in, FILE *out, struct bitmap_info *bitmap)
{
    // assumes input file is positioned at beginning of bitmap data
    // handles both grayscale and color data
    // depth that I've seen are 1, 2, 4, 16
    // writes to PGM (P5) file for grayscale image and PPM (P6 file) for color image
    unsigned char out_data, red, green, blue;
    uint16_t in_data;
    int color;
    int bitpos, i, j, emptybits;

    color = (bitmap->depth > 8) ? 1 : 0;

    // P6 for color, P5 for grayscale
    // max value per byte for color pics is scaled to 8 bits
    fprintf(out, "P%d\n%d %d\n%d\n", color ? 6 : 5,
            bitmap->width, bitmap->height, color ? 255 : (1<<bitmap->depth)-1);

    emptybits = bitmap->bytes_per_row * 8 - bitmap->width * bitmap->depth;
    bitpos = 16; // force reading on first pass through loop
    for (i = 0; i < bitmap->height; i++)
    {
        for (j = 0; j < bitmap->width; j++)
        {
            // read two bytes at a time mainly to deal with color images
            while (bitpos > 15)
            {
                bitpos -= 16;
                read_bytes(in, &in_data, sizeof(in_data));
                // in PGM, 0x00 is black and the large value is white
                // ipod seems to be the reverse so need to invert the bits
                if (! color)
                    in_data ^= 0xffff;

                // for grayscale, don't want to treat as an int
                // so need to "unflip" it on little endian machines
                // for color, we treat as an int so need to flip on big endian
                if ((! color && ! big_endian) || (color && big_endian))
                    in_data = _flip16(in_data);
            }

            if (color)
            {
                // 16-bit is RGB color encoded 5-6-5
                // scale it all up to 8 bits

                // need to occasionally swap bytes
                if (bitmap->color_flipped)
                    in_data = _flip16(in_data);

                blue = (in_data & 0x1f) << 3;
                in_data >>= 5;
                green = (in_data & 0x3f) << 2;
                in_data >>= 6;
                red = in_data << 3;

                write_bytes(out, &red, 1);
                write_bytes(out, &green, 1);
                write_bytes(out, &blue, 1);
            }
            else
            {
                // get next bitmap->depth number of bits
                // move bits we care about to the right
                // we assume groups of bits don't cross byte boundaries
                // that is the depth is always either 1, 2, 4, or 8
                out_data = in_data >> (16 - bitpos - bitmap->depth);

                // zero out everything we don't want
                out_data &= (1 << bitmap->depth) - 1;

                // write the byte out
                write_bytes(out, &out_data, sizeof(out_data));
            }

            // increment our position
            bitpos += bitmap->depth;
        }
        bitpos += emptybits;
    }
    // read remaining padding bytes for completeness
    while (bitpos > 16)         /* must be 16, we initialized with 16 */
    {
        bitpos -= 16;
        read_bytes(in, &in_data, sizeof(in_data));
    }
}

static int read_pnm_header(FILE *in, struct bitmap_info *bitmap, char *new_depth)
{
    // reads a PGM/PPM header from the file
    // fills in the bitmap struct
    // leaves file pointer at beginning of image data
    // grayscale images must have new_depth specified (a number as a string)
    // returns 1 on success, 0 otherwise
    int type, maxval = 0;
#define MAX_HEADER_SIZE 1023
    char buf[MAX_HEADER_SIZE+1]; // leave space for trailing NULL character
    char *incoming = buf;       // location of incoming data
    int incomment = 0;
    int c;
    char ch;

    memset(bitmap, 0, sizeof(*bitmap));
    buf[0] = '\0';              // begin with null string
    while ((sscanf(buf, "P%d %u %u %u%c", &type, &bitmap->width, &bitmap->height, &maxval, &ch) != 5))
    {
        if ((incoming >= buf+sizeof(buf)-1) || // out of space
            ((c = fgetc(in)) == EOF) || // read failure
            (!isascii(c)))     // hit a non-alphanum character
        {
            error("Image header could not be read");
        }

        if (incomment)
        {
            if (c == '\n')
            {
                incomment = 0;
                *incoming++ = ' '; // add whitespace
            }
            // else we skip
        }
        else if (c == '#')
            incomment = 1;      // begin comment
        else if (isspace(c) && incoming != buf && isspace(incoming[-1]))
            ;                   // multiple spaces, skip this one
        else
            *incoming++ = c;   // character is okay to add to buffer

        *incoming = '\0';       // null terminate
    }

    // calculate number of bits for depth from maxval
    for (bitmap->original_depth = 0; maxval != 0; bitmap->original_depth++)
        maxval = maxval >> 1;

    if (type == 6)
        bitmap->depth = 16;
    else if (type == 5)
    {
        // check for new grayscale depth
        if (! new_depth)
            error("Grayscale bitmaps must have a depth specified");
        bitmap->depth = atoi((char *)new_depth);
        if ((bitmap->depth != 1) && (bitmap->depth != 2) && (bitmap->depth != 4))
            error("Invalid bit depth %d given (must be 1 or 2 (or 4 - only for iPod photo))", bitmap->depth);
    }
    else
        error("Wrong type of image. Check that it is an appropriate P5 or P6 image");

    // fill in the calculated values in the struct
    bitmap->bytes_per_row = ((int)(((bitmap->width*bitmap->depth+7)/8+3)/4))*4;
    bitmap->length = bitmap->height * bitmap->bytes_per_row;
    return 1;
}

static void write_bitmap(FILE *out, FILE *in, struct bitmap_info *bitmap)
{
    // writes bitmap data to out in ipod format reading data from input file in PGM/PPM format
    uint16_t packed_rgb;
    int bitpos, i, j, emptybits;
    unsigned char in_data, out_data, red, green, blue;
    int color = (bitmap->depth == 16);
    char zero = 0;

    if (! color && (bitmap->original_depth > bitmap->depth))
        info("Rescaling bitmap depth from %d bits to %d bits", bitmap->original_depth, bitmap->depth);

    emptybits = bitmap->bytes_per_row * 8 - bitmap->width * bitmap->depth;
    out_data = 0;
    bitpos = 0;
    for (i = 0; i < bitmap->height; i++)
    {
        for (j = 0; j < bitmap->width; j++)
        {
            if (color)
            {
                read_bytes(in, &red, 1);
                read_bytes(in, &green, 1);
                read_bytes(in, &blue, 1);

                // reduce to 5-6-5 bits
                red >>= (bitmap->original_depth - 5);
                green >>= (bitmap->original_depth - 6);
                blue >>= (bitmap->original_depth - 5);

                // combine
                packed_rgb = red << 11 | green << 5 | blue;
                flip16(&packed_rgb, NULL);
                write_bytes(out, &packed_rgb, sizeof(packed_rgb));
            }
            else
            {
                while (bitpos > 7)
                {
                    write_bytes(out, &out_data, sizeof(out_data));
                    out_data = 0;
                    bitpos -= 8;
                }

                read_bytes(in, &in_data, sizeof(in_data));
                in_data ^= 0xff; // invert for ipod

                // take only the most significant bits for the depth we need
                in_data >>= (bitmap->original_depth - bitmap->depth);

                // zero out everything we don't want
                in_data &= (1 << bitmap->depth) - 1;

                // set the appropriate bits in out_data
                out_data |= (in_data << (8-bitpos-bitmap->depth));
                bitpos += bitmap->depth;
            }
        }

        if (color)
        {
            // write filler bytes
            for (j = 0; j < emptybits / 8; j++)
                write_bytes(out, &zero, 1);
        }
        else
            bitpos += emptybits;
    }
    // write out any remaining bytes
    if (!color)
    {
        while (bitpos > 0)
        {
            write_bytes(out, &out_data, sizeof(out_data));
            out_data = 0;
            bitpos -= 8;
        }
    }
}

static void info_bitmap(FILE *out, FILE *in, struct resource *resource)
{
    struct picture_header header;
    struct bitmap_info bitmap;

    read_bytes_from(in, 0, &header, sizeof(header));
    FIX_PICTURE_HEADER(&header);
    COPY_BITMAP(&bitmap, &header);
    fprintf(out, " height=%d width=%d depth=%d",
            bitmap.height, bitmap.width, bitmap.depth);
    UNUSED_VAR(resource);
}

static void decode_bitmap(FILE *in, char *out_filename)
{
    FILE *out;
    struct picture_header header;
    struct picture_header2 header2;
    struct bitmap_info bitmap;

    read_bytes_from(in, 0, &header, sizeof(header));
    FIX_PICTURE_HEADER(&header);
    COPY_BITMAP(&bitmap, &header);
    bitmap.color_flipped = 0;
    if (!valid_bitmap(&bitmap))
    {
        read_bytes_from(in, 0, &header2, sizeof(header2));
        FIX_PICTURE_HEADER2(&header2);
        COPY_BITMAP(&bitmap, &header2);
        if (!valid_bitmap(&bitmap))
            error("Invalid bitmap file while writing %s!", out_filename);

        if (header2.z1 || header2.z2 || header2.z3)
        {
            debug("*** %s: z's not all zero: z1: 0x%x, z2: 0x%x, z3: 0x%x ***",
                  out_filename, header2.z1, header2.z2, header2.z3);
        }
    }
    else
    {
        // these may be spacer bits. seems to always be zero
        if (header.z1 || header.z2 || header.z3)
        {
            debug("*** %s: z's not all zero: z1: 0x%x, z2: 0x%x, z3: 0x%x ***",
                  out_filename, header.z1, header.z2, header.z3);
            // z3 not always zero anymore
            if (header.z3)
                bitmap.color_flipped = 1;
        }
    }

    // change extension from pgm to ppm for color bitmaps
    if (bitmap.depth > 8)
        *(strrchr(out_filename, 'g')) = 'p';

    if (! (out = fopen(out_filename, "w")))
        perror_exit("fopen");

    read_bitmap(in, out, &bitmap);
    fclose(out);
}

static void encode_bitmap(FILE *in, char *file, void *arg)
{
    // arg is new depth for bitmap (as a char *)
    // the depth in the input file is usually set to 255 (8 bits) by programs
    // Graphics programs don't seem to write files with less than 8 bits
    FILE *out;
    struct picture_header header;
    struct bitmap_info bitmap;

    // read pic PGM/PPM
    if (! read_pnm_header(in, &bitmap, (char *)arg))
        return;

    dump_bitmap_info(&bitmap);
    if (! (out = fopen(file, "w")))
        perror_exit("fopen");

    memset(&header, 0, sizeof(header));
    COPY_BITMAP(&header, &bitmap);
    FIX_PICTURE_HEADER(&header); // for writing out header
    write_bytes(out, &header, sizeof(header)); // write header
    write_bitmap(out, in, &bitmap); // and then bitmap bits
    fclose(out);
}

static void encode_boot(FILE *in, char *file, void *arg)
{
    // boot picture pseudo resource contains just the bitmap bits
    FILE *out;
    struct bitmap_info bitmap;

    // read pic PGM/PPM
    if (! read_pnm_header(in, &bitmap, arg))
        return;

    dump_bitmap_info(&bitmap);
    if (! (out = fopen(file, "w")))
        perror_exit("fopen");

    write_bitmap(out, in, &bitmap); // write bitmap
    fclose(out);
}

static void decode_menu(FILE *in, char *file)
{
    struct property_list list;
    int32_t num_elems;

    memset(&list, 0, sizeof(list));

    read_bytes(in, &num_elems, sizeof(num_elems));
    flip32(&num_elems, NULL);
    if (num_elems > MAX_ELEMENTS)
        error("Too many elements (%d) in %s", num_elems, file);

    for (list.num_elems = 0; list.num_elems < num_elems; list.num_elems++)
    {
        read_property_fields(in, &list.elems[list.num_elems],
                             resource_type_menu, NUM_ELEM(resource_type_menu));
    }
    write_property_file(file, &list);
}

static void encode_menu(FILE *in, char *file, void *arg)
{
    struct property_list list;
    FILE *out;
    int i;
    int32_t num_elems;

    read_property_file(in, &list);

    if (! (out = fopen(file, "w")))
        perror_exit("fopen");

    num_elems = list.num_elems;
    flip32(&num_elems, NULL);
    write_bytes(out, &num_elems, sizeof(num_elems));
    for (i = 0; i < list.num_elems; i++)
    {
        write_property_fields(out, &list.elems[i], resource_type_menu, NUM_ELEM(resource_type_menu));
    }
    fclose(out);
    UNUSED_VAR(arg);
}

static void decode_type(FILE *in, char *file)
{
    struct property_list list;
    memset(&list, 0, sizeof(list));
    list.num_elems = 1;
    read_property_fields(in, &list.elems[0], resource_type_type, NUM_ELEM(resource_type_type));
    write_property_file(file, &list);
}

static void encode_type(FILE *in, char *file, void *arg)
{
    struct property_list list;
    FILE *out;
    read_property_file(in, &list);

    if (list.num_elems != 1)
        error("A type file requires a single element");

    if (! (out = fopen(file, "w")))
        perror_exit("fopen");

    write_property_fields(out, &list.elems[0], resource_type_type, NUM_ELEM(resource_type_type));
    fclose(out);
    UNUSED_VAR(arg);
}

#ifdef IPOD_ADVANCED_USER
static void decode_tcmd(FILE *in, char *file)
{
    FILE *out;
    int i;

    struct resource_type_tcmd tcmd;
    if (! (out = fopen(file, "w")))
        perror_exit("fopen");

    read_bytes(in, &tcmd.size_unk, sizeof(tcmd.size_unk));
    read_bytes(in, &tcmd.z1, sizeof(tcmd.z1));
    read_bytes(in, &tcmd.num_elems, sizeof(tcmd.num_elems));

    fprintf(out, "size_unk = 0x%x(%d)\n", tcmd.size_unk, tcmd.size_unk);
    if (tcmd.z1 != 0)
        printf("*** for %s z1 != 0 (0x%x(%d))\n", file, tcmd.z1, tcmd.z1);
    fprintf(out, "num_elems = 0x%x(%d)\n", tcmd.num_elems, tcmd.num_elems);

    tcmd.elems = calloc(tcmd.num_elems, sizeof(*tcmd.elems));

    for (i = 0; i < tcmd.num_elems; i++)
    {
        read_bytes(in, &tcmd.elems[i], sizeof(*tcmd.elems));
        fprintf(out, "\t%d = 0x%x(%d)\n", i, tcmd.elems[i], tcmd.elems[i]);
    }

    read_bytes(in, &tcmd.size2, sizeof(tcmd.size2));
    read_bytes(in, &tcmd.t2, sizeof(tcmd.t2));
    if (tcmd.t2 != 2)
        printf("*** for %s t2 != 2 (0x%x(%d))\n", file, tcmd.t2, tcmd.t2);
    read_bytes(in, &tcmd.t3, sizeof(tcmd.t3));
    read_bytes(in, &tcmd.t4, sizeof(tcmd.t4));
    read_bytes(in, &tcmd.z2, sizeof(tcmd.z2));
    if (tcmd.z2 != 0)
        printf("*** for %s z2 != 0 (0x%x(%d))\n", file, tcmd.z2, tcmd.z2);

    fprintf(out, "size2 = 0x%x(%d)\n", tcmd.size2, tcmd.size2);
    fprintf(out, "t3 = 0x%x(%d)\n", tcmd.t3, tcmd.t3);
    fprintf(out, "t4 = 0x%x(%d)\n", tcmd.t4, tcmd.t4);

    tcmd.nums = calloc(tcmd.size2, sizeof(*tcmd.nums));
    for (i = 0; i < tcmd.size2; i++)
    {
        read_bytes(in, &tcmd.nums[i], sizeof(*tcmd.nums));
        fprintf(out, "\t%d = 0x%x(%d) diff=%d\n", i, tcmd.nums[i], tcmd.nums[i],
                (i > 0 ? tcmd.nums[i]-tcmd.nums[i-1] : 0));
    }


    read_bytes(in, &tcmd.size3, sizeof(tcmd.size3));
    read_bytes(in, &tcmd.z3, sizeof(tcmd.z3));
    if (tcmd.z3 != 1)
        printf("*** for %s z3 != 1 (0x%x(%d))\n", file, tcmd.z3, tcmd.z3);

    fprintf(out, "size3 = 0x%x(%d)\n", tcmd.size3, tcmd.size3);

    tcmd.addrs = calloc(tcmd.size3, sizeof(*tcmd.addrs));
    /*
    i= 0;
    while (! feof(in))
    {
        _read_bytes(in, &tcmd.addrs[0], sizeof(*tcmd.addrs), 0);
        if (feof(in))
            break;
        if (tcmd.addrs[0] < 0x4000)
            i++;
    }
    fprintf(out, "i=%d\n", i);
    */
    for (i = 0; i < tcmd.size3; i++)
    {
        read_bytes(in, &tcmd.addrs[i], sizeof(*tcmd.addrs));
        fprintf(out, "\t%d = 0x%x(%u) %s\n", i, tcmd.addrs[i], tcmd.addrs[i],
                (tcmd.addrs[i] % 4 == 0 && tcmd.addrs[i] <0x347d30? "*" : ""));
    }
    fprintf(out, "*** Now at 0x%lx\n", ftell(in));
    free(tcmd.nums);
    free(tcmd.elems);
    free(tcmd.addrs);
    fclose(out);
}

static void decode_view(FILE *in, char *file)
{
    struct property_list list;
    int32_t num_elem;
    int i;
    uint32_t t1;
    struct resource_type_view_elem view;
    struct properties *props;
    char block[16];
    char item[16];
    struct resource_type *res_type;

    read_bytes(in, &num_elem, 4);
    flip32(&num_elem, NULL);
    if (num_elem > MAX_ELEMENTS)
        error("Too many elements (%d) in %s", num_elem, file);

    memset(&list, 0, sizeof(list));

    for (list.num_elems = 0; list.num_elems < num_elem; list.num_elems++)
    {
        props = &list.elems[list.num_elems];

        read_bytes(in, &view, sizeof(view));
        FIX_RESOURCE_TYPE_VIEW(&view);

        res_type = get_resource_type(view.type);
        add_property(props, "type", TYPE, *((int *)res_type->name), NULL);
        add_property(props, "z0", HEX, view.z0, NULL);
        add_property(props, "id", ID, view.id, NULL);
        add_property(props, "z1", HEX, view.z1, NULL);
        add_property(props, "z2", HEX, view.z2, NULL);
        add_property(props, "z3", HEX, view.z3, NULL);
        add_property(props, "z4", HEX, view.z4, NULL);

        for (i = 0; i < 4; i++)
        {
            FIX_RESOURCE_TYPE_VIEW_TEMP(&view.tmp[i]);
            snprintf(block, sizeof(block), "block%d", i);
            snprintf(item, sizeof(item), "%s-x1", block);
            add_property(props, item, HEX, view.tmp[i].a1, NULL);
            snprintf(item, sizeof(item), "%s-x2", block);
            add_property(props, item, HEX, view.tmp[i].a2, NULL);
            snprintf(item, sizeof(item), "%s-x3", block);
            add_property(props, item, HEX, view.tmp[i].a3, NULL);
            snprintf(item, sizeof(item), "%s-x4", block);
            add_property(props, item, HEX, view.tmp[i].a4, NULL);
        }

        // some have extra fields, different number doesn't seem to be any
        // consistent pattern

        // +4 for the length of the size field which isn't included in the size
        for (i = 0; i < (int)(view.size+4-sizeof(view))/4; i++)
        {
            snprintf(item, sizeof(item), "t%d", i);
            read_bytes(in, &t1, sizeof(t1));
            flip32(&t1, NULL);
            add_property(props, item, HEX, t1, NULL);
        }
    }
    write_property_file(file, &list);
}
#endif

static void _do_decode(struct resource_type *res_type, FILE *in,
                       const char *output_dir, const char *base_filename)
{
    char out_name[MAX_FILENAME];
    snprintf(out_name, sizeof(out_name), "%s/%s.%s", output_dir, base_filename, res_type->extension);
    goto_byte(in, 0);
    if (res_type->decode)
        (res_type->decode)(in, out_name);
    else
        error("Cannot decode resources of type '%s'", res_type->type);

    // sanity check. test that we are at end of the file indicating we
    // actually read/decoded it all
    if (! at_eof(in))
        warning("Did not completely decode %s", base_filename);
}

// ---------------------------------------------------------------------------------

static void try_map_unmap(int map, char *source_dir, char *source, char *dest, char *extension)
{
    char *dest_dir;
    char filename[MAX_FILENAME];
    FILE *in, *out;
    if (map)
    {
        dest_dir = MAP_DIR;
    }
    else
    {
        // swap the source and dest for unmapping
        char *tmp = dest;
        dest = source;
        source = tmp;
        dest_dir = DATA_DIR;
    }

    snprintf(filename, sizeof(filename), "%s/%s.%s", source_dir, source, extension);
    if ((in = fopen(filename, "r")))
    {
        info("Mapping file %s to %s", source, dest);

        snprintf(filename, sizeof(filename), "%s/%s.%s", dest_dir, dest, extension);
        if (! (out = fopen(filename, "w")))
            perror_exit("fopen");

        // copy source to dest
        copy_bytes(out, in, 0, fsize(in));
        fclose(out);
        fclose(in);
    }
}

static void do_map_unmap(char *dir, char *map_filename, int fatal_error, int map)
{
    FILE *mapfile;
    char line[BUFFER_SIZE], type[5];
    int rsrcID;
    char generic_name[BUFFER_SIZE];
    char rsrc[BUFFER_SIZE], generic[BUFFER_SIZE];
    int lineno = 0;
    char ch;
    int i;

    check_isdir(dir);

    info("Using map file %s", map_filename);
    if (! (mapfile = fopen(map_filename, "r")))
    {
        if (fatal_error)
            perror_exit("fopen");
        else
            info("Map file not found. Not performing filename map.");
        return;
    }

    if (map)
        mkdir(MAP_DIR, DIR_PERMISSIONS);
    else
        mkdir(DATA_DIR, DIR_PERMISSIONS);
        
    while (! feof(mapfile))
    {
        if ( ! fgets(line, sizeof(line), mapfile))
        {
            if (feof(mapfile))
                return;
            perror_exit("fgets");
        }
        lineno++;

        // skip empty lines
        if (sscanf(line, " %c", &ch) != 1)
            continue;           // line has no non-whitespace character

        if (sscanf(line, "%4s %d %s", type, &rsrcID, generic_name) != 3)
            error("Reading map file on line %d:\n  %s", lineno, line);

        // add hyphen for generic name to separate type from name
        snprintf(generic, sizeof(generic), "%s-%s", type, generic_name);

        // adding hyphen here would look strange for negative numbers
        snprintf(rsrc, sizeof(rsrc), "%s%d", type, rsrcID);

        // pretty brute force approach to try to map with filenames with
        // every possible extension that we use
        for (i = 0; i < NUM_ELEM(FILE_EXTENSIONS); i++)
            try_map_unmap(map, dir, rsrc, generic, FILE_EXTENSIONS[i]);
    }

    fclose(mapfile);
}

static void do_map(int argc, char **argv)
{
    do_map_unmap(argv[0], argc > 1 ? argv[1] : RESOURCE_MAP_FILE, 1, 1);
}

static void do_unmap(int argc, char **argv)
{
    do_map_unmap(argv[0], argc > 1 ? argv[1] : RESOURCE_MAP_FILE, 1, 0);
}

static void do_dump_fonts(int argc, char **argv)
{
    struct font f;
    struct char_metric cm;
    struct unicode_group group;
    struct bitmap_info bitmap;
    unsigned int pos, font_begin, prev_end, i;
    int count = 0;
    unsigned int num_metrics;
    uint16_t tmp;
    unsigned char filename[MAX_FILENAME];
    FILE *out;
    firmware_t *fw;

    mkdir("ipod-fonts", DIR_PERMISSIONS);

    fw = open_firmware(argv[0], "r", 1);

    // search the retailos image for fonts
    // pretty dumb search
    prev_end = 0;
    for (pos = fw->retailos->code_offset;
         pos < fw->retailos->code_offset + fw->retailos->length;
         pos += 4)
    {
        _read_bytes_from(fw->file, pos, &f.cinfo, sizeof(f.cinfo), 0);
        FIX_CHARACTER_INFO(&f.cinfo);
        if (feof(fw->file))
            break;

        if (memcmp(f.cinfo.magic, FONT_MAGIC, sizeof(FONT_MAGIC)) != 0)
            continue;

        count++;
        if (prev_end && prev_end != pos - 1)
            debug("*** Gap from 0x%x-0x%x", prev_end+1, pos-1);

        info("Found font at 0x%x", pos);

        if (f.cinfo.z1 != 1)
            debug("\t**** z1 != 1 (%d)", f.cinfo.z1);

#if PRINT_BASIC_FONT_INFO
        debug("\tChars: 0x%x-0x%x", f.cinfo.first_char, f.cinfo.last_char);
        debug("\tNum Chars: 0x%x (%d)", f.cinfo.num_chars, f.cinfo.num_chars);
        debug("\tNum Groups: 0x%x (%d)", f.cinfo.num_groups, f.cinfo.num_groups);
#endif

#if PRINT_ALL_FONT_INFO
        debug("\tCharacter Groups");
#endif
        for (i = 0; i < f.cinfo.num_groups; i++)
        {
            read_bytes(fw->file, &group, sizeof(group));
            FIX_UNICODE_GROUP(&group);

#if PRINT_ALL_FONT_INFO
            debug("\t\t\%3d: %d-%d \t0x%x-0x%x", i,
                  group.index, group.index+group.length-1,
                  group.start, group.start+group.length-1);
#endif
        }

        font_begin = ftell(fw->file);

        read_bytes(fw->file, &f.finfo, sizeof(f.finfo));
        FIX_FONT_INFO(&f.finfo);
            
        if (f.finfo.z1 != 4)
            printf("\t***font_info.z1 != 4 (0x%x)\n", f.finfo.z1);
        if (f.finfo.finfo_size != sizeof(struct font_info))
            printf("\t*** font_info.finfo_size != 0x%x (0x%x)\n",
                   (int)sizeof(struct font_info), f.finfo.finfo_size);
        if (f.finfo.num_chars != f.cinfo.num_chars)
            printf("\t****Number of chars in font (%d) and character info (%d) are different??\n",
                   f.finfo.num_chars, f.cinfo.num_chars);

#if PRINT_BASIC_FONT_INFO
        printf("\tFont: %s\n", f.finfo.font_name);
        printf("\tSize: %d\n", f.finfo.size);
        printf("\tStyle = 0x%x\n", f.finfo.style);
        printf("\tDepth = %d\n", f.finfo.depth);
        printf("\tFixed width = %s\n", f.finfo.fixed_width ? "yes" : "no");
        printf("\tWidth = %d\n", f.finfo.width);
        printf("\tLine Height = %d\n", f.finfo.line_height);
        printf("\tDescent = %d\n", f.finfo.descent);
        printf("\tt6 = %d\n", f.finfo.t6);
        printf("\tCharmap End = 0x%x\n", f.finfo.charmap_end);
        printf("\tMetrics End = 0x%x\n", f.finfo.metrics_end);
#endif

        // character mappings
#if PRINT_ALL_FONT_INFO
        printf("\tCharacter mapping:\n");
#endif
        for (i = 0; i < f.finfo.num_chars; i++)
        {
#if PRINT_ALL_FONT_INFO
            if (i % 10 == 0)
                printf("\t\t%3d: ", i);
#endif

            read_bytes(fw->file, &tmp, sizeof(tmp));
            if(big_endian)
                tmp = _flip16(tmp);
#if PRINT_ALL_FONT_INFO
            printf("%4d, ", tmp);
            if (i % 10 == 9)
                printf("\n");
#endif
        }
#if PRINT_ALL_FONT_INFO
        printf("\n");
#endif

        // Not sure why sometimes there's an extra 2 bytes of zeros
        while (font_begin + f.finfo.charmap_end > (unsigned int)ftell(fw->file))
        {
            read_bytes(fw->file, &tmp, 2);
            if (tmp != 0)
                printf("*** Read non-zero bytes after charmap (0x%x)\n", tmp);
        }
            

        // character metrics don't exist for fixed width fonts
        num_metrics = (f.finfo.metrics_end - f.finfo.charmap_end) / sizeof(cm);
        if (num_metrics * sizeof(cm) != f.finfo.metrics_end - f.finfo.charmap_end)
            printf("*** Num metrics to read is not a whole number??\n");

#if PRINT_ALL_FONT_INFO
        printf("\tCharacter metrics (left, right, width, left_indent)\n");
#endif
        for (i = 0; i < num_metrics; i++)
        {
#if PRINT_ALL_FONT_INFO
            if (i % 3 == 0)
                printf("\t\t%3d: ", i);
#endif

            read_bytes(fw->file, &cm, sizeof(cm));
            FIX_CHAR_METRICS(&cm);
#if PRINT_ALL_FONT_INFO
            printf("(%d, %d, %d, %d),\t", cm.left_offset, cm.right_offset,
                   cm.width, cm.left_indent);
            if (i % 3 == 2)
                printf("\n");
#endif
        }
#if PRINT_ALL_FONT_INFO
        printf("\n");
#endif
            
        read_bytes(fw->file, &f.bitmap, sizeof(f.bitmap));
        FIX_FONT_BITMAP(&f.bitmap);
        COPY_BITMAP(&bitmap, &f.bitmap);

        if (!valid_bitmap(&bitmap))
            error("Invalid bitmap in font!");

        prev_end = ftell(fw->file) + f.bitmap.length - 1;
#if PRINT_BASIC_FONT_INFO
        info("\tFont bitmap from 0x%lx-0x%x", ftell(fw->file), prev_end);
#endif

        if (f.bitmap.z1 || f.bitmap.z2)
            debug("*** z1 or z2 is non-zero (z1=%d, z2=%d) ***", f.bitmap.z1, f.bitmap.z2);

#if PRINT_BASIC_FONT_INFO
        dump_bitmap_info(&bitmap);
#endif

        snprintf(filename, sizeof(filename), "ipod-fonts/font-0x%x.pgm", pos);
        if (! (out = fopen(filename, "w")))
            perror_exit("fopen");
        read_bitmap(fw->file, out, &bitmap);
        fclose(out);
        debug("End font at 0x%x\n", prev_end);
    }
    info("Total fonts found: %d\n", count);
    close_firmware(fw);

    UNUSED_VAR(argc);
}

static void dump_resource_data(firmware_t *fw, unsigned int data_begin,
                               struct resource_block *block, int block_num)
{
    // dumps all resource data beginning from data_begin with the info in block
    struct resource_type *res_type;
    struct resource *res;
    struct resource_section *section;
    unsigned int i, j, num;
    unsigned char filename[MAX_FILENAME], fullpath[MAX_FILENAME];
    FILE *rsrc_file;

    num = 0;
    for (i = 0; i < block->header.num_sections; i++)
    {
        section = &block->sections[i];
        res_type = get_resource_type(section->type);
        for (j = 0; j < block->sections[i].num_items; j++, num++)
        {
            res = &block->resources[num];

            // for everything but primary resource block, block_num == 0,
            // attach the resource block number to front of filename
            if (block_num)
                snprintf(filename, sizeof(filename), "%d-%.4s%d", block_num, res_type->name, res->id);
            else
                snprintf(filename, sizeof(filename), "%.4s%d", res_type->name, res->id);

            // write out raw data
            snprintf(fullpath, sizeof(fullpath), "%s/%s.rsrc", RAW_RESOURCES_DIR, filename);
            if (! (rsrc_file = fopen(fullpath, "w+")))
                perror_exit("fopen");
            write_bytes(rsrc_file, fw->data+res->offset+data_begin, res->size);

            // print out info about the resource
            if (fw->resource_file)
            {
                fprintf(fw->resource_file,
                        "%s: id=0x%x loc=0x%x size=%d",
                        filename, res->id, res->offset+data_begin, res->size);
                if (res_type->info)
                {
                    goto_byte(rsrc_file, 0);
                    res_type->info(fw->resource_file, rsrc_file, res);
                }
                fprintf(fw->resource_file, "\n");
            }

            // try to decode the data if we know how
            if (res_type->decode)
                _do_decode(res_type, rsrc_file, DATA_DIR, filename);
            fclose(rsrc_file);
        }
    }
}

static void write_resource_data(firmware_t *fw, unsigned int block_start,
                                struct resource_block *block, int maxsize, FILE **update_files)
{
    // writes all resource data for the block replacing any that need to
    struct resource_section *section;
    struct resource *res;
    unsigned int i, j, num;
    int data_begin;
    int replaced = 0;
    char *data, *ptr;

    data_begin = block_start + block->header.length;

    // create the new image in memory first
    data = malloc(maxsize);
    memset(data, 0, maxsize);

    ptr = data;
    num = 0;
    for (i = 0; i < block->header.num_sections; i++)
    {
        section = &block->sections[i];
        for (j = 0; j < block->sections[i].num_items; j++, num++)
        {
            res = &block->resources[num];

            if (update_files[num])
            {
                // using new data
                res->size = fsize(update_files[num]);
                read_bytes_from(update_files[num], 0, ptr, res->size);
                replaced++;
            }
            else
            {
                // use original data
                fw_read_from(fw, data_begin + res->offset, ptr, res->size);
            }

            res->offset = ptr - data; // set new offset to current position
            ptr += DWORD_ALIGN(res->size);   // we align each resource on dword boundary

            FIX_RESOURCE(res); // for writing out
        }
    }

    // write new resource header info and data
    fw_write_to(fw, block_start + sizeof(struct resource_block_header) +
                block->header.num_sections * sizeof(struct resource_section),
                block->resources, sizeof(struct resource) * num);
    fw_write_to(fw, data_begin, data, maxsize);

    info("\tWrote new resource block, replacing %d resources", replaced);
    free(data);
}

static void process_resource_block(firmware_t *fw, unsigned int start_loc, int block_num,
                                   int process_block, char *update_dir)
{
    // can both dump and update the resource block at start_loc
    // dumps resources to files if dump=1
    // updates resources if update_dir is non-null
    // will leave the file position at the end of the resource data
    //
    // the block_num is something we assign to distinguish the various blocks
    // we use 1..n for the first n contiguous blocks containing language info
    // and number 0 is reserved for the last block (the main block) containing
    // the English text, the bitmaps, and other language-independent resources

    struct resource *res;
    struct resource_type *res_type;
    struct resource_block block;
    struct resource_section *section;
    unsigned int total, num, i, j, data_begin, lastend = 0, maxsize, cursize, newsize;
    uint32_t tmp;
    char filename[MAX_FILENAME];
    FILE **update_files = NULL;
    int updating = 0;
    int processing = 0;

    if ((process_block < 0) || (process_block == block_num))
        processing = 1;

    // read block header
    fw_read_from(fw, start_loc, &block.header, sizeof(block.header));
    FIX_RESOURCE_BLOCK_HEADER(&block.header);

    if (block.header.z1 != 0x03)
        warning("block header.z1 != 0x03 (=0x%x)", block.header.z1);
    if (processing)
        debug("Resource block at 0x%x length=0x%x, num=%d",
             start_loc, block.header.length, block.header.num_sections);

    // read list of resources types
    total = 0;
    block.sections = calloc(block.header.num_sections, sizeof(struct resource_section));
    for (i = 0; i < block.header.num_sections; i++)
    {
        section = &block.sections[i];
        fw_read(fw, section, sizeof(*section));
        FIX_RESOURCE_SECTION(section);
        total += section->num_items;
    }
    block.resources = calloc(total, sizeof(struct resource));
    update_files = calloc(total, sizeof(*update_files)); // rely on calloc setting it all to NULL

    // read list of all resources grouped by type
    num = 0;
    newsize = 0;
    for (i = 0; i < block.header.num_sections; i++)
    {
        section = &block.sections[i];
        res_type = get_resource_type(section->type);
        if (processing)
            debug("\tResource type=%.4s num=%d t1=0x%x",
                 res_type->name, section->num_items, section->t1);
        if (fw_tell(fw)-start_loc != section->start)
            warning("start of resource section (0x%x) != current location (0x%lx)",
                    section->start, fw_tell(fw)-start_loc);
        for (j = 0; j < block.sections[i].num_items; j++, num++)
        {
            res = &block.resources[num];
            // *** res can be null?
            if (! res)
                error("res is null!");

            fw_read(fw, res, sizeof(*res));
            FIX_RESOURCE(res);
            if (lastend && res->offset != lastend)
                warning("gap in resources from 0x%x-0x%x", lastend, res->offset-1);

            lastend = DWORD_ALIGN(res->offset + res->size);

            if (processing && update_dir)
            {
                // check if exists a file to update this resource
                if (block_num)
                    snprintf(filename, sizeof(filename), "%s/%d-%.4s%d.rsrc",
                             update_dir, block_num, res_type->name, res->id);
                else
                    snprintf(filename, sizeof(filename), "%s/%.4s%d.rsrc",
                             update_dir, res_type->name, res->id);

                if ((update_files[num] = fopen(filename, "r")))
                {
                    info("\t => Found resource file %s to write", filename);
                    newsize += DWORD_ALIGN(fsize(update_files[num]));
                    updating = 1;
                }
                else
                    newsize += DWORD_ALIGN(res->size);
            }
        }
    }

    if (fw_tell(fw)-start_loc != block.header.length)
        error("Length in header (0x%x) != bytes read (0x%lx)",
              block.header.length, fw_tell(fw)-start_loc);

    data_begin = fw_tell(fw);
    if (processing)
        debug("\tRead %d resources. Resource list ends at 0x%x.", num, data_begin);

    // calculate what the maximum size of the block could be by going to end
    // and finding the first non-zero int32 as the limit of how much more we can squeeze in
    cursize = DWORD_ALIGN(block.resources[num-1].offset+block.resources[num-1].size);
    maxsize = cursize;
    fw_seek(fw, data_begin + cursize);
    tmp = 0;
    while (tmp == 0)
    {
        fw_read(fw, &tmp, sizeof(tmp));
        if (tmp == 0)
            maxsize += sizeof(tmp);
    }

    // the "slack" extra number of bytes for more data is approximate because of the
    // needed dword alignment for each resource means we may have more bytes
    if (processing)
    {
        debug("\tBlock uses 0x%x bytes for data. Max size is 0x%x. Slack is (about) %d bytes.",
              cursize, maxsize, maxsize-cursize);
        if (update_dir)
        {
            // check if there's anything to update
            if (updating)
            {
                info("\tCurrent block size: 0x%x. New size: 0x%x. Max size: 0x%x.\t(%s)",
                     cursize, newsize, maxsize, (newsize > maxsize) ? "NOT OK!" : "OK");
                if (newsize > maxsize)
                    warning("Cannot update this resource block due to size constraints.");
                else
                    write_resource_data(fw, start_loc, &block, maxsize, update_files);
            }
        }
        else
            dump_resource_data(fw, data_begin, &block, block_num);
    }

    // must goto end of resource data to prepare for reading next block
    fw_seek(fw, data_begin + maxsize);

    // cleanup
    for (i = 0; i < total; i++)
    {
        if (update_files[i])
            fclose(update_files[i]);
    }
    free(update_files);
    free(block.sections);
    free(block.resources);
}

static int is_resource_block(firmware_t *fw, int loc)
{
    struct resource_block_header header;
    char type[4];

    // not sure the best way to identifying a resource block
    // so this is a heuristic
    fw_read_from(fw, loc, &header, sizeof(header));
    FIX_RESOURCE_BLOCK_HEADER(&header);
    fw_read(fw, type, sizeof(type));

    if (known_resource_type(type) &&
        header.z1 == 0x03 &&
        header.length < 0xffff && header.length > 0 &&
        header.num_sections < 50 && header.num_sections > 0)
    {
        return 1;
    }
    return 0;
}

static unsigned int find_resource_begin(firmware_t *fw)
{
    unsigned int start_loc;
    unsigned int end = fw->retailos->code_offset + fw->retailos->length;
    
    info("Searching for resources...");
    // we assume block is dword-aligned, which significantly speeds up search
    for (start_loc = fw->retailos->code_offset; start_loc < end; start_loc += 4)
    {
        if (is_resource_block(fw, start_loc))
        {
            info("Possible start of resources at 0x%x", start_loc);
            return start_loc;
        }
    }

    error("Could not find resource block");
    exit(-1);                   /* unreachable */
}

static void process_resources(firmware_t *fw, int process_block, char *update_dir)
{
    // loops through all resource blocks in a firmware
    // can dump or update the blocks
    // if process_block is > 0, then dumps/updates only that block number from the
    //    language resources with the first language resource block number = 1
    // if process_block = 0, then dumps/updates only the primary block
    // if process_block = -1, then dumps/updates all blocks

    unsigned int start_loc = 0;
    int block = 1;

    // language resources are all together in one block
    // do mindless search for first one
    start_loc = find_resource_begin(fw);
    while (1)
    {
        process_resource_block(fw, start_loc, block++, process_block, update_dir);
        start_loc = DWORD_ALIGN(fw_tell(fw));
        // expect next resource block to begin immediately here
        if (! is_resource_block(fw, start_loc))
        {
            info("Language resources end at 0x%x", start_loc);
            break;
        }
    }

    // The main block for English+bitmaps and other resources is located after all the rest
    // doesn't directly follow so need to do a search for it
    info("Searching for primary resource block...");
    for (; ; start_loc += 4)
    {
        if (is_resource_block(fw, start_loc))
        {
            // we use block num 0 to refer to the primary block
            info("Found primary resource block at 0x%x", start_loc);
            process_resource_block(fw, start_loc, 0, process_block, update_dir);
            return;
        }
    }
    error("Could not find primary resource block.");
    exit(-1);                   /* unreachable */
}

static void process_bootloader(firmware_t *fw, char *update_dir)
{
    // extracts and writes pictures in the bootloader
    // only works for pre-4th gen non-encrypted bootloaders
    unsigned int start_loc;
    unsigned int end;
    int num = 0;
    struct bootloader_picture_footer footer;
    struct bitmap_info bitmap;
    unsigned char filename[MAX_FILENAME], fullpath[MAX_FILENAME];
    FILE *file, *decode_file;

    if (! fw->softupdt)
    {
        // e.g. for ipodlinux firmware
        warning("No bootloader image in firmware");
        return;
    }

    if (FOURTH_GEN(fw))
    {
        info("Skipping bootloader for 4th generation firmware");
        return;
    }

    info("Searching for bootloader pictures");
    end = fw->softupdt->code_offset + fw->softupdt->length;
    for (start_loc = fw->softupdt->code_offset;
         start_loc < end - sizeof(footer);
         start_loc += 4)
    {
        fw_read_from(fw, start_loc, &footer, sizeof(footer));
        FIX_PICTURE_FOOTER(&footer);
        COPY_BITMAP(&bitmap, &footer);
        if (valid_bitmap(&bitmap))
        {
            // treat these pictures as a pseudo resource of type "boot"
            // resource data is only the bitmap bits, no footer
            snprintf(filename, sizeof(filename), "boot%d", num);

            // print out info about the resource
            if (fw->resource_file)
            {
                fprintf(fw->resource_file,
                        "%s: id=0x%x loc=0x%x size=%d",
                        filename, num, start_loc-bitmap.length, bitmap.length+(int)sizeof(footer));
                fprintf(fw->resource_file, " height=%d width=%d depth=%d",
                        bitmap.height, bitmap.width, bitmap.depth);
                fprintf(fw->resource_file, "\n");
            }

            if (footer.z1 || footer.z2)
                debug("*** z1=0x%x, z2=0x%x", footer.z1, footer.z2);
            debug("\tz3=0x%x(%d), z4=0x%x(%d)", footer.z3, footer.z3, footer.z4, footer.z4);

            snprintf(fullpath, sizeof(fullpath), "%s/%s.rsrc",
                     (update_dir ? update_dir : RAW_RESOURCES_DIR), filename);
            if (update_dir)
            {
                if ((file = fopen(fullpath, "r")))
                {
                    info("\t => Found %s to write", fullpath);

                    // check it's the right size before writing
                    if (fsize(file) != bitmap.length)
                    {
                        warning("Image in %s is not the expected size (%d). Skipping...",
                                fullpath, bitmap.length);
                    }
                    else
                    {
                        fw_write_from_file(fw, start_loc-bitmap.length, file, bitmap.length);
                    }
                    fclose(file);
                }
            }
            else
            {
                if (! (file = fopen(fullpath, "w+")))
                    perror_exit("fopen");
                write_bytes(file, fw->data+start_loc-bitmap.length, bitmap.length);

                // decode the data to PGM file. no color pictures on pre-4th gen firmwares
                snprintf(fullpath, sizeof(fullpath), "%s/%s.pgm", DATA_DIR, filename);
                if (! (decode_file = fopen(fullpath, "w")))
                    perror_exit("fopen");
                goto_byte(file, 0);
                read_bitmap(file, decode_file, &bitmap);
                fclose(decode_file);
                fclose(file);
            }
            num++;
        }
    }
    info("Found %d bootloader pictures", num);
}

static void process_firmwares(char *filename, int maxnum, char *update_dir)
{
    // dumps/updates firmwares from a file (e.g. an updater)
    // if maxnum == 0, all firmwares are dumped. if positive, only dumps/writes
    // at most that many firmwares
    int begin = 0, num = 0;
    unsigned int length;
    FILE *file;
    FILE *firm;
    char firmname[MAX_FILENAME];
    firmware_t fw;

    info("Searching for firmwares in %s...", filename);
    if (! (file = fopen(filename, (update_dir ? "r+" : "r"))))
        perror_exit("fopen");

    fw.file = file;
    for (begin = 0; !feof(file) && /* search to end of file */
             (begin < MAX_FILE_SEARCH) && /* up to some point and just give up */
             (maxnum <= 0 || num < maxnum); /* stop if we've found enough firmwares */
         begin++)
    {
        if (is_firmware(file, begin))
        {
            fw.num_images = 0;

            // find end of this firmware
            read_firmware_info(&fw, begin);
            length = firmware_size(&fw);

            // replace or extract the firmware
            snprintf(firmname, sizeof(firmname), "%s/firmware-%d.bin",
                     (update_dir ? update_dir : FIRMWARE_DIR), num);
            if (update_dir)
                insert_file(file, firmname, begin, length);
            else
            {
                if (! (firm = fopen(firmname, "w")))
                    perror_exit("fopen");
                info("Dumping firmware from 0x%x to 0x%x (length 0x%x) to %s", begin, begin+length-1, length, firmname);
                copy_bytes(firm, file, begin, length);
                fclose(firm);
            }

            num++;
            begin = begin + length; // save lots of time by skipping over just dumped firmware
        }
    }
    if (num == 0)
        warning("No firmwares were found!");
    fclose(file);
}

static void do_write(int argc, char **argv)
{
    // this is the meat of the program that actually changes stuff
    // file to update is argv[0], directory to get files for updating is argv[1]
    // argv[2] can be a block number to limit updating to
    // if file to update is a firmware, then updates resources
    // otherwise, tries to update the firmwares inside the file (e.g. an updater)
    char *dir = argv[1];
    firmware_t *fw;

    check_isdir(dir);
    fw = open_firmware(argv[0], "r+", 0);
    if (fw)
    {
        // is a firmware -- write out resources
        int block = -1;
        if (argc > 2)
            block = atoi(argv[2]);
        process_resources(fw, block, dir);
        process_bootloader(fw, dir);
        update_checksum(fw, 1);
        close_firmware(fw);
    }
    else
    {
        // write out firmwares
        int max = 0;
        if (argc > 2)
            max = atoi(argv[2]);
        process_firmwares(argv[0], max, dir);
    }
}

static void do_extract(int argc, char **argv)
{
    // extract resources from a firmware
    // extract firmwares from a file (e.g. updater)
    // an extra optional argument of a number can be given
    firmware_t *fw = open_firmware(argv[0], "r", 0);
    int arg = 0;
    if (argc > 1)
        arg = atoi(argv[1]);
    if (fw)
    {
        // is a firmware -- dump out resources
        mkdir(RAW_RESOURCES_DIR, DIR_PERMISSIONS);
        mkdir(DATA_DIR, DIR_PERMISSIONS);
        if (! (fw->resource_file = fopen(RESOURCE_INFO_FILE, "w")))
            perror_exit("fopen");
        
        process_resources(fw, arg, NULL);
        process_bootloader(fw, NULL);
        do_map_unmap(DATA_DIR, RESOURCE_MAP_FILE, 0, 1);
        close_firmware(fw);
    }
    else
    {
        // else try to find and extract out any firmware in the file
        mkdir(FIRMWARE_DIR, DIR_PERMISSIONS);
        process_firmwares(argv[0], arg, NULL);
    }
}

static void do_checksum(int argc, char **argv)
{
    // update checksum and print info about the firmware
    firmware_t *fw = open_firmware(argv[0], "r+", 1);
    update_checksum(fw, 1);
    close_firmware(fw);
    UNUSED_VAR(argc);
}

static void do_info(int argc, char **argv)
{
    firmware_t *fw = open_firmware(argv[0], "r", 1);
    // get info about firmware
    update_checksum(fw, 0);
    close_firmware(fw);
    UNUSED_VAR(argc);
}

static void do_dump_images(int argc, char **argv)
{
    // dump firmware images out
    firmware_t *fw = open_firmware(argv[0], "r", 1);
    char filename[MAX_FILENAME];
    FILE *out;
    unsigned int i;
    for (i = 0; i < fw->num_images; i++)
    {
        snprintf(filename, sizeof(filename), "image-%d", i);
        out = fopen(filename, "w");
        if (!out)
            perror_exit("fopen");
        info("Dumping image to %s", filename);
        write_bytes(out, fw->data + fw->images[i].code_offset, fw->images[i].length);
    }
    close_firmware(fw);
    UNUSED_VAR(argc);
}

static void do_prepare(int argc, char **argv)
{
    FILE *in;
    char out_name[MAX_FILENAME], *ptr;
    int id, block;
    struct resource_type *res_type;
    char *arg = NULL;
    char *name = argv[0];

    if (argc > 1)
        arg = argv[1];

    info("Preparing %s", name);
    if (! (in = fopen(name, "r")))
        perror_exit("fopen");

    // save name before we try to parse filename
    strncpy(out_name, name, sizeof(out_name));
    res_type = parse_resource_filename(name, &block, &id);
    if (!res_type->encode)
    {
        warning("Cannot handle resources of type '%s'", res_type->name);
        return;
    }

    // generate new name by removing whatever extension and appending .rsrc
    ptr = strrchr(out_name, '.');
    strcpy(ptr, ".rsrc");
    info("Writing resource data to %s", out_name);

    (res_type->encode)(in, out_name, arg);
    // sanity check. test that we are at end of the file
    // indicating we actually read through it all
    if (! at_eof(in))
        warning("Did not completely encode %s", name);
    fclose(in);
}

static void do_decode(int argc, char **argv)
{
    FILE *in;
    int i;
    char name[MAX_FILENAME], *ptr;
    struct resource_type *res_type;
    int id, block;
    for (i = 0; i < argc; i++)
    {
        strncpy(name, argv[i], sizeof(name));
        info("Decoding %s", name);
        if (! (in = fopen(name, "r")))
            perror_exit("fopen");

        res_type = parse_resource_filename(name, &block, &id);

        // remove extension
        ptr = strrchr(name, '.');
        if (strcmp(ptr, ".rsrc") != 0)
            error("Invalid extension %s, .rsrc file expected", ptr);
        *ptr = '\0';
        _do_decode(res_type, in, ".", name);
        fclose(in);
    }
}

int main(int argc, char **argv)
{
    char *cmd;
    int i;
    struct command_map command_table[] =
    {
        { "extract", "x", do_extract, 1 },
        { "write", "w", do_write, 2 },

        { "checksum", "c", do_checksum, 1 },
        { "info", "i", do_info, 1 },
        { "prepare", "p", do_prepare, 1 },
        { "decode", "d", do_decode, 1 },

        { "map", "m", do_map, 1 },
        { "unmap", "u", do_unmap, 1 },

        // non-advertised commands for my use
        { "fonts", NULL, do_dump_fonts, 1 }, /* maybe merge into extract */
        { "dump-images", NULL, do_dump_images, 1 },
    };

    check_endian();
    program_name = argv[0];     // save program name for usage()
    if (argc == 1)
        return usage();
    else
    {
        cmd = argv[1];
        // remove the program and command name
        argc -= 2;
        argv += 2;
    }

    // find what command was given and dispatch to that handler
    for (i = 0; i < NUM_ELEM(command_table); i++)
    {
        if ((strcmp(cmd, command_table[i].name) == 0) ||
            (command_table[i].shortname && (strcmp(cmd, command_table[i].shortname) == 0)))
        {
            if (argc < command_table[i].min_args)
                return usage();
            else
                (command_table[i].main)(argc, argv);
            return 0;
        }
    }

    return usage();
}
