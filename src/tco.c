#include "stdlib.h"
#include "errno.h"
#include "string.h"
#include "stdbool.h"
#include <png.h>
#include <bps/bps.h>
#include <bps/screen.h>
#include <bps/event.h>
#include <bps/navigator.h>
#include "touchcontroloverlay.h"
#include <queue.h>
#include <math.h>
#include <cJSON.h>

/* Maximum number of defined controls */
#define MAX_TCO_CONTROLS 8

/* Logging */
#define DEBUGLOG(message, ...) fprintf(stderr, "%s(%s@%d): " message "\n", __FILE__, __FUNCTION__, __LINE__, ##__VA_ARGS__);

/* Callback types */
typedef int (*HandleKeyFunc)(int sym, int mod, int scancode, unsigned short unicode, int event);
typedef int (*HandleDPadFunc)(int angle, int event);
typedef int (*HandleTouchFunc)(int dx, int dy);
typedef int (*HandleMouseButtonFunc)(int button, int mask, int event);
typedef int (*HandleTapFunc)();
typedef int (*HandleTouchScreenFunc)(int x, int y, int tap, int hold);

const static int TAP_THRESHOLD = 150000000L;
const static int JITTER_THRESHOLD = 10;

/* Control types enumeration */
typedef enum {
    KEY, /* Used to provide keyboard input */
    DPAD, /* Provides angle and magnitude from center (0 east, 90 north, 180 west, 270 south) */
    TOUCHAREA, /* Used to provide relative mouse motion */
    MOUSEBUTTON, /* Used to provide mouse button state */
    TOUCHSCREEN /* Provides: mouse move, left click tap and right click tap-hold */
} tco_control_type;

/* Forward declaration of pointer to structure */
typedef struct tco_window * tco_window_t;
typedef struct tco_label_window * tco_label_window_t;
typedef struct tco_configuration_window * tco_configuration_window_t;
typedef struct tco_label * tco_label_t;
typedef struct tco_control * tco_control_t;
typedef struct png_reader * png_reader_t;
typedef struct touch_owner * touch_owner_t;

struct touch_owner {
    tco_control_t control;
    int touch_id;
    SLIST_ENTRY(touch_owner) link;
};

/* TCO window */
struct tco_window {
    tco_context_t m_context;
    screen_window_t m_window;
    screen_window_t m_parent;
    int m_size[2]; /* width, height */
    int m_alpha; /* 0..255 */
};

/* TCO label window */
struct tco_label_window {
    struct tco_window m_baseWindow;
    int m_offset[2]; /* x, y */
    float m_scale[2]; /* x, y */
};

/* TCO configuration window */
struct tco_configuration_window {
    struct tco_window m_baseWindow;
    tco_control_t m_selected;
    int m_startPos[2];
    int m_endPos[2];
};

/* TCO label */
struct tco_label {
    int m_x;
    int m_y;
    int m_width;
    int m_height;
    char * m_image_file;
    tco_control_t m_control;
    tco_label_window_t m_label_window;
};

/* TCO control */
struct tco_control {
    /* Control type */
    tco_control_type m_type;

    /* Control id */
    int m_id;

    int m_x;
    int m_y;
    int m_width;
    int m_height;

    /* Control image properties */
    int m_srcWidth;
    int m_srcHeight;

    tco_context_t m_context;

    int m_touchId;

    union {
        struct {
            int m_last_x;
            int m_last_y;
            long long m_touchDownTime;
        } touch_area; /* For touch areas */
        struct {
            int m_start_x;
            int m_start_y;
            long long m_touchScreenStartTime;
            bool m_touchScreenInMoveEvent;
            bool m_touchScreenInHoldEvent;
        } touch_screen; /* For touch screen */
    } m_state;

    /* Control label */
    tco_label_t m_label;

    /* Control specific properties */
    union {
        struct {
            int m_symbol;
            int m_modifier;
            int m_scancode;
            int m_unicode;
        } key; /* KEY properties */
        struct {
            int m_mask;
            int m_button;
        } mouse; /* MOUSEBUTTON properties */
        struct {
            int m_tapSensitive;
        } touch; /* TOUCHAREA properties */

    } m_properties;
};

/* TCO context */
struct tco_context {
    screen_context_t m_screenContext;
    tco_configuration_window_t m_configWindow;

    /* Defined controls */
    tco_control_t * m_controls;
    int m_numControls;

    SLIST_HEAD(touch_owners, touch_owner) m_touch_owners;

    /* Where to save user control settings*/
    char * m_user_control_path;

    /* Callbacks */
    HandleKeyFunc m_handleKeyFunc;
    HandleDPadFunc m_handleDPadFunc;
    HandleTouchFunc m_handleTouchFunc;
    HandleMouseButtonFunc m_handleMouseButtonFunc;
    HandleTapFunc m_handleTapFunc;
    HandleTouchScreenFunc m_handleTouchScreenFunc;
};

struct png_reader {
    tco_context_t m_context;
    screen_pixmap_t m_pixmap;
    screen_buffer_t m_buffer;

    png_structp m_read;
    png_infop m_info;
    unsigned char* m_data; /* image data */
    png_bytep* m_rows;  /* pointers to PNG lines */
    int m_width; /* image width */
    int m_height; /* image height */
    int m_stride; /* image line width in buffer */
};

/* Utility functions */
static char * tco_read_text_file(const char * fileName)
{
    if(!fileName || fileName[0] == 0) {
        DEBUGLOG("No file to read");
        return NULL;
    }

    long len;
    char *buf = 0;
    FILE *fp = fopen(fileName, "r");
    while (fp)
    {
        if (0 != fseek(fp, 0, SEEK_END))
        {
            DEBUGLOG("%s (%d)", strerror(errno), errno);
            break;
        }
        len = ftell(fp);
        if (len == -1)
        {
            DEBUGLOG("%s (%d)", strerror(errno), errno);
            break;
        }
        if (0 != fseek(fp, 0, SEEK_SET))
        {
            DEBUGLOG("%s (%d)", strerror(errno), errno);
            break;
        }
        buf = (char *) calloc(1, len + 1);
        if (!buf)
        {
            DEBUGLOG("%s (%d)", strerror(errno), errno);
            break;
        }
        fread(buf, len, 1, fp);
        if (ferror(fp))
        {
            DEBUGLOG("%s (%d)", strerror(errno), errno);
            free(buf);
            buf = 0;
            break;
        }
        break;
    }
    if (fp)
    {
        fclose(fp);
    }
    return buf;
}

/* JSON functions */
static int tco_json_get_int(cJSON * object, const char * name)
{
    cJSON * value = cJSON_GetObjectItem(object, name);
    if (value != NULL && value->type == cJSON_Number) {
        return value->valueint;
    } else {
        DEBUGLOG("Could not get int (%s) from JSON", name);
    }
    return 0;
}

static int tco_json_set_int(cJSON * object, const char * name, int value) {
    cJSON * p = cJSON_CreateNumber(value);
    if(p) {
        cJSON_AddItemToObject(object, name, p);
        return TCO_SUCCESS;
    } else {
        DEBUGLOG("Could not set int (%s) in JSON", name);
        return TCO_FAILURE;
    }
}

static const char * tco_json_get_str(cJSON * object, const char * name)
{
    cJSON * value = cJSON_GetObjectItem(object, name);
    if (value != NULL && value->type == cJSON_String) {
        return value->valuestring;
    } else {
        DEBUGLOG("Could not get str (%s) from JSON", name);
    }
    return 0;
}

static int tco_json_set_str(cJSON * object, const char * name, const char * value) {
    cJSON * p = cJSON_CreateString(value);
    if(p) {
        cJSON_AddItemToObject(object, name, p);
        return TCO_SUCCESS;
    } else {
        DEBUGLOG("Could not set str (%s) in JSON", name);
        return TCO_FAILURE;
    }
}

/* PNG reader functions */
static png_reader_t tco_png_reader_alloc(tco_context_t context) {
    png_reader_t png = (png_reader_t)calloc(1, sizeof(struct png_reader));
    if(png) {
        png->m_context = context;
    }
    return png;
}

static void tco_png_reader_free(png_reader_t png) {
    if(png) {
        int rc;
        if (png->m_read) {
            png_destroy_read_struct(&png->m_read,
                                    png->m_info ? &png->m_info : (png_infopp) 0,
                                    (png_infopp) 0);
        }
        if (png->m_pixmap) {
            rc = screen_destroy_pixmap(png->m_pixmap);
            if(rc) {
                DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
            }
        } else if (png->m_buffer) {
            rc = screen_destroy_buffer(png->m_buffer);
            if(rc) {
                DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
            }
        }
        free(png->m_rows);
        free(png->m_data);
        free(png);
    }
}

static bool tco_png_reader_read(png_reader_t png, const char * fileName) {
    if(!png || !fileName || fileName[0] == 0) {
        DEBUGLOG("No PNG file to read");
        return false;
    }

    bool result = false;
    FILE * file = NULL;
    while(true) {
        file = fopen(fileName, "r");
        if(!file) {
            DEBUGLOG("Could not open PNG file: %s (%d)", strerror(errno), errno);
            break;
        }

        png_byte header[8];
        if(fread(header, 1, sizeof(header), file) < sizeof(header)) {
            DEBUGLOG("Could not read PNG file: %s (%d)", strerror(errno), errno);
            break;
        }

        if (png_sig_cmp(header, 0, sizeof(header))) {
            DEBUGLOG("Invalid PNG signature");
            break;
        }

        png->m_read = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
        if (!png->m_read) {
            DEBUGLOG("Could not read PNG structure: %s (%d)", strerror(errno), errno);
            break;
        }

        png->m_info = png_create_info_struct(png->m_read);
        if (!png->m_info) {
            DEBUGLOG("Could not create PNG info structure: %s (%d)", strerror(errno), errno);
            break;
        }

        if (setjmp(png_jmpbuf(png->m_read))) {
            DEBUGLOG("Could not process PNG file: %s (%d)", strerror(errno), errno);
            break;
        }

        png_init_io(png->m_read, file);
        png_set_sig_bytes(png->m_read, sizeof(header));
        png_read_info(png->m_read, png->m_info);

        png->m_width = png_get_image_width(png->m_read, png->m_info);
        if (png->m_width <= 0 || png->m_width > 1024) {
            DEBUGLOG("Invalid PNG width: %d", png->m_width);
            break;
        }

        png->m_height = png_get_image_height(png->m_read, png->m_info);
        if (png->m_height <= 0 || png->m_height > 600) {
            DEBUGLOG("Invalid PNG height: %d", png->m_height);
            break;
        }

        png_byte color_type = png_get_color_type(png->m_read, png->m_info);
        if(PNG_COLOR_TYPE_RGBA != color_type) {
            DEBUGLOG("Invalid PNG color type %d, must be %d (in file %s)", color_type, PNG_COLOR_TYPE_RGBA, fileName);
            break;
        }
        png_byte bit_depth = png_get_bit_depth(png->m_read, png->m_info);
        if(8 != bit_depth) {
            DEBUGLOG("Invalid PNG bit depth %d, must be %d (in file %s)\n", bit_depth, 8, fileName);
            break;
        }

        png_read_update_info(png->m_read, png->m_info);

        const int channels = 4;
        png_set_palette_to_rgb(png->m_read);
        png_set_tRNS_to_alpha(png->m_read);
        png_set_bgr(png->m_read);
        png_set_expand(png->m_read);
        png_set_strip_16(png->m_read);
        png_set_gray_to_rgb(png->m_read);

        if (png_get_channels(png->m_read, png->m_info) < channels) {
            png_set_filler(png->m_read, 0xff, PNG_FILLER_AFTER);
        }

        png->m_data = (unsigned char*)calloc(1, png->m_width * png->m_height * channels);
        if(!png->m_data) {
            DEBUGLOG("%s (%d)", strerror(errno), errno);
            break;
        }

        int png_stride = png->m_width * channels;
        png->m_rows = (png_bytep*)calloc(1, png->m_height * sizeof(png_bytep));
        if(!png->m_rows) {
            DEBUGLOG("%s (%d)", strerror(errno), errno);
            break;
        }

        int i;
        for (i = png->m_height - 1; i >= 0; --i) {
            png->m_rows[i] = (png_bytep)(png->m_data + i * png_stride);
        }
        png_read_image(png->m_read, png->m_rows);

        int rc;
        int format = SCREEN_FORMAT_RGBA8888;
        int size[2] = {png->m_width, png->m_height};

        rc = screen_create_pixmap(&png->m_pixmap, png->m_context->m_screenContext);
        if(rc) {
            DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
            break;
        }

        rc = screen_set_pixmap_property_iv(png->m_pixmap,
                                           SCREEN_PROPERTY_FORMAT,
                                           &format);
        if(rc) {
            DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
            break;
        }

        rc = screen_set_pixmap_property_iv(png->m_pixmap,
                                           SCREEN_PROPERTY_BUFFER_SIZE,
                                           size);
        if(rc) {
            DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
            break;
        }

        rc = screen_create_pixmap_buffer(png->m_pixmap);
        if(rc) {
            DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
            break;
        }

        unsigned char *realPixels;
        int realStride;
        rc = screen_get_pixmap_property_pv(png->m_pixmap,
                                           SCREEN_PROPERTY_RENDER_BUFFERS,
                                           (void**)&png->m_buffer);
        if(rc) {
            DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
            break;
        }

        rc = screen_get_buffer_property_pv(png->m_buffer,
                                           SCREEN_PROPERTY_POINTER,
                                           (void **)&realPixels);
        if(rc) {
            DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
            break;
        }

        rc = screen_get_buffer_property_iv(png->m_buffer,
                                           SCREEN_PROPERTY_STRIDE,
                                           &realStride);
        if(rc) {
            DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
            break;
        }

        memset(realPixels, 0, realStride * png->m_height * sizeof(unsigned char));

        unsigned char * buffer_pixel_row = realPixels;
        unsigned char * png_pixel_row = png->m_data;
        for(i = 0; i < png->m_height; ++i) {
            memcpy(buffer_pixel_row, png_pixel_row, realStride);
            png_pixel_row += png_stride;
            buffer_pixel_row += realStride;
        }

        /* we copied the data - no need to keep the copy in memory */
        free(png->m_rows);
        png->m_rows = NULL;
        free(png->m_data);
        png->m_data = NULL;

        result = true;
        break;
    }

    if(file) {
        fclose(file);
    }
    return result;
}

static bool tco_label_window_initialize_from_png(tco_label_window_t label_window,
                                                 png_reader_t png,
                                                 int alpha);

static bool tco_label_load_image(tco_context_t context,
                                 tco_label_t label,
                                 int alpha) {
    bool result = true;
    if(label->m_image_file!=NULL && label->m_image_file[0]!='\0') {
        png_reader_t png = tco_png_reader_alloc(context);
        if(png) {
            if(tco_png_reader_read(png, label->m_image_file)) {
                result = tco_label_window_initialize_from_png(label->m_label_window,
                                                              png,
                                                              alpha);
            } else {
                result = false;
            }
            tco_png_reader_free(png);
        } else {
            result = false;
        }
    }
    return result;
}

/* TCO window functions */
static bool tco_window_set_parent(tco_window_t window,
                                  screen_window_t parent) {
    if(!window) {
        return false;
    }
    if (parent == window->m_parent) {
        return true;
    }

    int rc;
    if (parent != 0) {
        char buffer[256] = {0};

        rc = screen_get_window_property_cv(parent, SCREEN_PROPERTY_GROUP, 256, buffer);
        if (rc) {
            DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
            return false;
        }

        rc = screen_join_window_group(window->m_window, buffer);
        if (rc) {
            DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
            return false;
        }

        window->m_parent = parent;
    } else if (window->m_parent) {
        rc = screen_leave_window_group(window->m_window);
        if (rc) {
            DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
            return false;
        }
        window->m_parent = 0;
    }
    return true;
}

static bool tco_window_get_pixels(tco_window_t window,
                                  screen_buffer_t *buffer,
                                  unsigned char ** pixels,
                                  int *stride) {
    screen_buffer_t buffers[2];
    int rc = screen_get_window_property_pv(window->m_window,
                                           SCREEN_PROPERTY_RENDER_BUFFERS,
                                           (void**)buffers);
    if (rc) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }
    *buffer = buffers[0];

    rc = screen_get_buffer_property_pv(*buffer,
                                       SCREEN_PROPERTY_POINTER,
                                       (void **)pixels);
    if (rc) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }

    rc = screen_get_buffer_property_iv(*buffer,
                                       SCREEN_PROPERTY_STRIDE,
                                       stride);
    if (rc) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }

    return true;
}

static bool tco_window_init_ex(tco_window_t window,
                               tco_context_t context,
                               int width,
                               int height,
                               int alpha,
                               screen_window_t parent) {
    if(!window) {
        return false;
    }
    memset(window, 0, sizeof(struct tco_window));
    window->m_context = context;

    int rc;
    int format = SCREEN_FORMAT_RGBA8888;
    int usage = SCREEN_USAGE_NATIVE | SCREEN_USAGE_READ | SCREEN_USAGE_WRITE;

    window->m_size[0] = width;
    window->m_size[1] = height;
    window->m_alpha = alpha;

    rc = screen_create_window_type(&window->m_window,
                                    window->m_context->m_screenContext,
                                    SCREEN_CHILD_WINDOW);
    if (rc) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        window->m_window = 0;
        return false;
    }

    rc = screen_set_window_property_iv(window->m_window,
                                       SCREEN_PROPERTY_FORMAT,
                                       &format);
    if (rc) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }

    rc = screen_set_window_property_iv(window->m_window,
                                       SCREEN_PROPERTY_USAGE,
                                       &usage);
    if (rc) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }

    rc = screen_set_window_property_iv(window->m_window,
                                       SCREEN_PROPERTY_SIZE,
                                       window->m_size);
    if (rc) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }

    rc = screen_set_window_property_iv(window->m_window,
                                       SCREEN_PROPERTY_BUFFER_SIZE,
                                       window->m_size);
    if (rc) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }

    rc = screen_create_window_buffers(window->m_window, 1);
    if (rc) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }

    if (!tco_window_set_parent(window, parent))
    {
        return false;
    }

    return true;
}

static bool tco_window_init(tco_window_t window,
                            tco_context_t context,
                            screen_window_t parent) {
    int rc = screen_get_window_property_iv(parent,
                                           SCREEN_PROPERTY_BUFFER_SIZE,
                                           window->m_size);
    if (rc) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }
    window->m_alpha = 0xFF;
    return tco_window_init_ex(window,
                             context,
                             window->m_size[0],
                             window->m_size[1],
                             window->m_alpha,
                             parent);
}

static bool tco_window_set_z_order(tco_window_t window,
                                   int zOrder) {
    if(!window) {
        return false;
    }
    int rc = screen_set_window_property_iv(window->m_window,
                                           SCREEN_PROPERTY_ZORDER,
                                           &zOrder);
    if (rc) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }
    return true;
}

static bool tco_window_set_touch_sensitivity(tco_window_t window,
                                             int sensitivity) {
    if(!window) {
        return false;
    }
    sensitivity = (sensitivity > 0 ? SCREEN_SENSITIVITY_ALWAYS:SCREEN_SENSITIVITY_NEVER);
    int rc = screen_set_window_property_iv(window->m_window,
                                           SCREEN_PROPERTY_SENSITIVITY,
                                           &sensitivity);
    if (rc) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }
    return true;
}

static bool tco_window_set_visible(tco_window_t window,
                                   bool visible) {
    int is_visible = visible ? 1 : 0;
    int rc = screen_set_window_property_iv(window->m_window,
                                           SCREEN_PROPERTY_VISIBLE,
                                           &is_visible);
    if (rc) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }
    return true;
}

static bool tco_window_post(tco_window_t window,
                            screen_buffer_t buffer) {
    if(!window) {
        return false;
    }
    int dirtyRects[4] = {0, 0, window->m_size[0], window->m_size[1]};
    int rc = screen_post_window(window->m_window, buffer, 1, dirtyRects, 0);
    if(rc) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }
    return true;
}

static void tco_window_done(tco_window_t window) {
    if(!window) {
        return;
    }
    if (window->m_window) {
        int rc = screen_destroy_window(window->m_window);
        if(rc) {
            DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        }
    }
    memset(window, 0, sizeof(struct tco_window));
}


/* TCO label window functions */
static tco_label_window_t tco_label_window_alloc(tco_context_t context,
                                                 int width,
                                                 int height,
                                                 int alpha) {
    tco_label_window_t window = (tco_label_window_t)calloc(1, sizeof(struct tco_label_window));
    if(!tco_window_init_ex(&window->m_baseWindow,
                           context,
                           width,
                           height,
                           alpha,
                           NULL)) {
        free(window);
        return NULL;
    }
    if(!tco_window_set_z_order(&window->m_baseWindow, 6)) {
        tco_window_done(&window->m_baseWindow);
        free(window);
        return NULL;
    }
    if(!tco_window_set_touch_sensitivity(&window->m_baseWindow, 0)) {
        tco_window_done(&window->m_baseWindow);
        free(window);
        return NULL;
    }
    window->m_offset[0] = 0;
    window->m_offset[1] = 0;
    window->m_scale[0] = 1.0f;
    window->m_scale[1] = 1.0f;
    return window;
}

static void tco_label_window_free(tco_label_window_t window) {
    if(window) {
        tco_window_done(&window->m_baseWindow);
        free(window);
    }
}

static bool tco_label_window_move(tco_label_window_t window,
                                  int x,
                                  int y) {
    if(!window) {
        return false;
    }
    int position[] = {window->m_offset[0] + (x * window->m_scale[0]),
                      window->m_offset[1] + (y * window->m_scale[1])};
    int rc = screen_set_window_property_iv(window->m_baseWindow.m_window,
                                           SCREEN_PROPERTY_POSITION,
                                           position);
    if (rc) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }
    return true;
}

static bool tco_label_window_show_at(tco_label_window_t window,
                                     screen_window_t parent,
                                     int x,
                                     int y) {
    if(!window) {
        return false;
    }
    int rc = 0;
    if (parent && parent != window->m_baseWindow.m_parent) {
        int parentBufferSize[2];
        int parentSize[2];
        rc = screen_get_window_property_iv(parent,
                                           SCREEN_PROPERTY_POSITION,
                                           window->m_offset);
        if(rc) {
            DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
            return false;
        }

        rc = screen_get_window_property_iv(parent,
                                           SCREEN_PROPERTY_BUFFER_SIZE,
                                           parentBufferSize);
        if(rc) {
            DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
            return false;
        }

        rc = screen_get_window_property_iv(parent,
                                           SCREEN_PROPERTY_SIZE,
                                           parentSize);
        if(rc) {
            DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
            return false;
        }

        window->m_scale[0] = parentSize[0] / (float)parentBufferSize[0];
        window->m_scale[1] = parentSize[1] / (float)parentBufferSize[1];

        int newSize[] = {window->m_baseWindow.m_size[0] * window->m_scale[0],
                         window->m_baseWindow.m_size[1] * window->m_scale[1]};

        rc = screen_set_window_property_iv(window->m_baseWindow.m_window,
                                           SCREEN_PROPERTY_SIZE,
                                           newSize);
        if(rc) {
            DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
            return false;
        }
    }

    if (!tco_window_set_parent(&window->m_baseWindow, parent)) {
        return false;
    }

    tco_label_window_move(window, x, y);

    tco_window_set_visible(&window->m_baseWindow, true);

    return true;
}

static bool tco_label_window_initialize_from_png(tco_label_window_t label_window,
                                                 png_reader_t png,
                                                 int alpha) {
    if(!label_window) {
        return false;
    }
    int rc;
    screen_buffer_t buffer;
    unsigned char *pixels;
    int stride;
    if (!tco_window_get_pixels(&label_window->m_baseWindow,
                               &buffer,
                               &pixels,
                               &stride)) {
        DEBUGLOG("Unable to get window pixels");
        return false;
    }

    screen_buffer_t pixmapBuffer;
    rc = screen_get_pixmap_property_pv(png->m_pixmap,
                                       SCREEN_PROPERTY_RENDER_BUFFERS,
                                       (void**)&pixmapBuffer);
    if(rc != 0) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }

    const int fill_attribs[] = {
        SCREEN_BLIT_COLOR, 0x0,
        SCREEN_BLIT_END
    };
    rc = screen_fill(label_window->m_baseWindow.m_context->m_screenContext,
                     buffer,
                     fill_attribs);
    if(rc != 0) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }

    const int blit_attribs[] = {
            SCREEN_BLIT_SOURCE_X, 0,
            SCREEN_BLIT_SOURCE_Y, 0,
            SCREEN_BLIT_SOURCE_WIDTH, png->m_width,
            SCREEN_BLIT_SOURCE_HEIGHT, png->m_height,
            SCREEN_BLIT_DESTINATION_X, 0,
            SCREEN_BLIT_DESTINATION_Y, 0,
            SCREEN_BLIT_DESTINATION_WIDTH, label_window->m_baseWindow.m_size[0],
            SCREEN_BLIT_DESTINATION_HEIGHT, label_window->m_baseWindow.m_size[1],
            SCREEN_BLIT_TRANSPARENCY, SCREEN_TRANSPARENCY_SOURCE,
            SCREEN_BLIT_GLOBAL_ALPHA, (alpha == -1 ? label_window->m_baseWindow.m_alpha : alpha),
            SCREEN_BLIT_SCALE_QUALITY, SCREEN_QUALITY_NICEST,
            SCREEN_BLIT_END
    };
    rc = screen_blit(label_window->m_baseWindow.m_context->m_screenContext,
                     buffer,
                     pixmapBuffer,
                     blit_attribs);
    if(rc != 0) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }

    if(!tco_window_post(&label_window->m_baseWindow, buffer)) {
        return false;
    }
    return true;
}

/* Configuration window functions */
static bool tco_configuration_window_draw(tco_configuration_window_t window,
                                          bool show) {
    if(!window) {
        return false;
    }
    screen_buffer_t buffer;
    unsigned char *pixels;
    int stride;
    if (!tco_window_get_pixels(&window->m_baseWindow,
                               &buffer,
                               &pixels,
                               &stride)) {
        return false;
    }

    int y=0,x=0;
    const unsigned char back_alpha = 0x90;
    unsigned char c;
    const int cell_size = 16;
    for (y=0; y< window->m_baseWindow.m_size[1]; y++) {
        int t = y & cell_size;
        int h = y * stride;
        for (x=0; x < window->m_baseWindow.m_size[0]; x++) {
            int p = x * 4;
            c = ((x & cell_size) ^ t) ? 0xa0 : 0x80;
            pixels[h + p + 0] = c;
            pixels[h + p + 1] = c;
            pixels[h + p + 2] = c;
            pixels[h + p + 3] = back_alpha;
        }
    }

    if(!tco_window_post(&window->m_baseWindow, buffer)) {
        return false;
    }

    int i = 0;
    for(; i < window->m_baseWindow.m_context->m_numControls; ++i) {
        if(!tco_label_load_image(window->m_baseWindow.m_context,
                                 window->m_baseWindow.m_context->m_controls[i]->m_label,
                                 (show ? 0xff : -1))) {
            return false;
        }
    }
    return true;
}

static tco_configuration_window_t tco_configuration_alloc(tco_context_t context,
                                                          screen_window_t parent) {
    tco_configuration_window_t window = (tco_configuration_window_t)calloc(1, sizeof(struct tco_configuration_window));
    if(!tco_window_init(&window->m_baseWindow, context, parent)) {
        free(window);
        return NULL;
    }

    if(!tco_window_set_z_order(&window->m_baseWindow, 10)) {
        tco_window_done(&window->m_baseWindow);
        free(window);
        return NULL;
    }
    if(!tco_window_set_touch_sensitivity(&window->m_baseWindow, 1)) {
        tco_window_done(&window->m_baseWindow);
        free(window);
        return NULL;
    }

    if(!tco_configuration_window_draw(window, true)) {
        return NULL;
    }
    return window;
}

static tco_control_t tco_context_control_at(tco_context_t ctx,
                                            int x,
                                            int y);

static bool tco_control_move(tco_control_t control,
                             int dx,
                             int dy,
                             int max_x,
                             int max_y);

static int tco_configuration_window_run(tco_configuration_window_t window,
                                        screen_event_t screen_event) {
    int rc;
    int eventType;
    int touchId;
    bool releasedThisRound = false;

    if(screen_event!=NULL) {
        rc = screen_get_event_property_iv(screen_event,
                                          SCREEN_PROPERTY_TYPE,
                                          &eventType);
        if(rc) {
            DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
            return TCO_FAILURE;
        }

        /* Detect touchId */
        switch(eventType) {
        case SCREEN_EVENT_MTOUCH_TOUCH:
        case SCREEN_EVENT_MTOUCH_MOVE:
        case SCREEN_EVENT_MTOUCH_RELEASE:
            rc = screen_get_event_property_iv(screen_event,
                                              SCREEN_PROPERTY_TOUCH_ID,
                                              &touchId);
            if(rc) {
                DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
                return TCO_FAILURE;
            }
            break;
        default:
            return TCO_UNHANDLED;
        }

        /* Only handle first touch */
        if(touchId == 0) {
            switch(eventType)
            {
            case SCREEN_EVENT_MTOUCH_TOUCH:
                if (!window->m_selected) {
                    rc = screen_get_event_property_iv(screen_event,
                                                      SCREEN_PROPERTY_SOURCE_POSITION,
                                                      window->m_startPos);
                    if(rc) {
                        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
                        return TCO_FAILURE;
                    }

                    window->m_selected = tco_context_control_at(window->m_baseWindow.m_context,
                                                                window->m_startPos[0],
                                                                window->m_startPos[1]);
                    if(window->m_selected) {
                        window->m_endPos[0] = window->m_startPos[0];
                        window->m_endPos[1] = window->m_startPos[1];
                    } else {
                        window->m_endPos[0] = window->m_startPos[0] = 0;
                        window->m_endPos[1] = window->m_startPos[1] = 0;
                    }
                }
                break;
            case SCREEN_EVENT_MTOUCH_MOVE:
                if (window->m_selected) {
                    rc = screen_get_event_property_iv(screen_event,
                                                      SCREEN_PROPERTY_SOURCE_POSITION,
                                                      window->m_endPos);
                    if(rc) {
                        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
                        return TCO_FAILURE;
                    }
                }
                break;
            case SCREEN_EVENT_MTOUCH_RELEASE:
                if (window->m_selected) {
                    releasedThisRound = true;
                    rc = screen_get_event_property_iv(screen_event,
                                                      SCREEN_PROPERTY_SOURCE_POSITION,
                                                      window->m_endPos);
                    if(rc) {
                        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
                        return TCO_FAILURE;
                    }
                }
                break;
            default:
                return TCO_UNHANDLED;
            }
        }
    } /* screen_event!=NULL */

    if (releasedThisRound) {
        window->m_selected = NULL;
        window->m_endPos[0] = window->m_startPos[0] = 0;
        window->m_endPos[1] = window->m_startPos[1] = 0;
    } else if (window->m_selected) {
        int deltaX = window->m_endPos[0] - window->m_startPos[0];
        int deltaY = window->m_endPos[1] - window->m_startPos[1];

        int absDeltaX = abs(deltaX);
        int absDeltaY = abs(deltaY);

        int maxDelta = max(absDeltaX, absDeltaY);

        if (maxDelta > 0 ||
            (screen_event == NULL && maxDelta != 0)) {
            window->m_startPos[0] = window->m_endPos[0];
            window->m_startPos[1] = window->m_endPos[1];
            if(!tco_control_move(window->m_selected,
                             deltaX,
                             deltaY,
                             window->m_baseWindow.m_size[0],
                             window->m_baseWindow.m_size[1])) {
                return TCO_FAILURE;
            }
        }
    }

    return TCO_SUCCESS;
}

static void tco_configuration_window_free(tco_configuration_window_t window) {
    if (window) {
        tco_configuration_window_draw(window, false);
        tco_window_done(&window->m_baseWindow);
        free(window);
    }
}

/* Label functions */
static tco_label_t tco_label_alloc(tco_context_t context,
                                   tco_control_t control,
                                   int x,
                                   int y,
                                   int width,
                                   int height,
                                   int alpha,
                                   const char * image) {
    tco_label_t label = (tco_label_t)calloc(1, sizeof(struct tco_label));
    label->m_control = control;
    label->m_x = x;
    label->m_y = y;
    label->m_width = width;
    label->m_height = height;
    label->m_label_window = tco_label_window_alloc(context,
                                                   width,
                                                   height,
                                                   alpha);
    if(image) {
        label->m_image_file = strdup(image);
        tco_label_load_image(context, label, -1); /* use image alpha */
    }
    return label;
}

static void tco_label_free(tco_label_t label) {
    if(label) {
        label->m_control = NULL;
        tco_label_window_free(label->m_label_window);
        free(label->m_image_file);
        free(label);
    }
}

static bool tco_label_draw(tco_label_t label,
                           screen_window_t window,
                           int x,
                           int y) {
    return tco_label_window_show_at(label->m_label_window,
                                    window,
                                    label->m_x + x,
                                    label->m_y + y);
}

static bool tco_label_move(tco_label_t label,
                           int x,
                           int y) {
    return tco_label_window_move(label->m_label_window,
                                 label->m_x + x,
                                 label->m_y + y);
}


/* Control functions */
static tco_control_t tco_control_alloc(tco_context_t context,
                                       int id,
                                       const char * controlType,
                                       int x,
                                       int y,
                                       int width,
                                       int height) {
    tco_control_t control = calloc(1, sizeof(struct tco_control));
    control->m_context = context;
    control->m_id = id;
    if(strcmp(controlType, "key") == 0) {
        control->m_type = KEY;
    } else if (strcmp(controlType, "dpad") == 0) {
        control->m_type = DPAD;
    } else if (strcmp(controlType, "toucharea") == 0) {
        control->m_type = TOUCHAREA;
    } else if (strcmp(controlType, "mousebutton") == 0) {
        control->m_type = MOUSEBUTTON;
    } else if (strcmp(controlType, "touchscreen") == 0) {
        control->m_type = TOUCHSCREEN;
    } else {
        control->m_type = -1;
    }
    control->m_x = x;
    control->m_y = y;
    control->m_width = width;
    control->m_height = height;
    control->m_srcWidth = width;
    control->m_srcHeight = height;
    control->m_touchId = -1;
    return control;
}

static void tco_control_free(tco_control_t control) {
    tco_label_free(control->m_label);
    free(control);
}

static bool tco_control_move(tco_control_t control,
                             int dx,
                             int dy,
                             int max_x,
                             int max_y) {
    if(!control) {
        return false;
    }
    if(dx == 0 && dy == 0) {
        return true;
    }
    control->m_x += dx;
    control->m_y += dy;
    if (control->m_x <= 0)
        control->m_x = 0;
    if (control->m_y <= 0)
        control->m_y = 0;
    if (control->m_x + control->m_width >= max_x)
        control->m_x = max_x - control->m_width;
    if (control->m_y + control->m_height >= max_y)
        control->m_y = max_y - control->m_height;
    return tco_label_move(control->m_label, control->m_x, control->m_y);
}

static bool tco_control_draw_label(tco_control_t control,
                                   screen_window_t window) {
    if(!control) {
        return false;
    }
    if(control->m_label!=NULL) {
        return tco_label_draw(control->m_label,
                              window,
                              control->m_x,
                              control->m_y);
    }
    return true;
}

static bool tco_control_in_bounds(tco_control_t control,
                                  int x,
                                  int y) {
    if(!control) {
        return false;
    }
    return (x >= control->m_x &&
            x <= control->m_x + control->m_width &&
            y >= control->m_y &&
            y <= control->m_y + control->m_height);
}

static bool tco_control_handle_touch(tco_control_t control,
                                     tco_context_t context,
                                     int type,
                                     int touchId,
                                     int x,
                                     int y,
                                     long long timestamp) {
    if(!control) {
        return false;
    }
    if (control->m_touchId != -1 &&
        control->m_touchId != touchId) {
        /*  We have a contact point set and this isn't it. */
        return false;
    }

    if (control->m_touchId == -1) {
        /*  Don't handle orphaned release events. */
        if (type == SCREEN_EVENT_MTOUCH_RELEASE) {
            return false;
        }

        if (!tco_control_in_bounds(control, x, y)) {
            return false;
        }

        /*  This is a new touch point that we should start handling */
        control->m_touchId = touchId;

        switch (control->m_type)
        {
        case KEY:
            if(context->m_handleKeyFunc) {
                context->m_handleKeyFunc(control->m_properties.key.m_symbol,
                                         control->m_properties.key.m_modifier,
                                         control->m_properties.key.m_scancode,
                                         control->m_properties.key.m_unicode,
                                         TCO_KB_DOWN);
            }
            break;
        case DPAD:
            if(context->m_handleDPadFunc) {
                int angle = atan2((y - control->m_y - control->m_height / 2.0f),
                                  (x - control->m_x - control->m_width / 2.0f)) * 180 / M_PI;
                context->m_handleDPadFunc(angle, TCO_KB_DOWN);
            }
            break;
        case TOUCHAREA:
            control->m_state.touch_area.m_touchDownTime = timestamp;
            control->m_state.touch_area.m_last_x = x;
            control->m_state.touch_area.m_last_y = y;
            break;
        case MOUSEBUTTON:
            if(context->m_handleMouseButtonFunc) {
                context->m_handleMouseButtonFunc(control->m_properties.mouse.m_button,
                                                 control->m_properties.mouse.m_mask,
                                                 TCO_MOUSE_BUTTON_DOWN);
            }
            break;
        case TOUCHSCREEN:
            control->m_state.touch_screen.m_start_x = x;
            control->m_state.touch_screen.m_start_y = y;
            control->m_state.touch_screen.m_touchScreenStartTime = timestamp;
            break;
        default:
            break;
        }
    } else {
        if (!tco_control_in_bounds(control, x, y)) {
            /* Act as if we received a key up */
            switch (control->m_type)
            {
            case KEY:
                if(context->m_handleKeyFunc) {
                    context->m_handleKeyFunc(control->m_properties.key.m_symbol,
                                             control->m_properties.key.m_modifier,
                                             control->m_properties.key.m_scancode,
                                             control->m_properties.key.m_unicode,
                                             TCO_KB_UP);
                }
                break;
            case DPAD:
                if(context->m_handleDPadFunc) {
                    int angle = atan2((y - control->m_y - control->m_height / 2.0f),
                                      (x - control->m_x - control->m_width / 2.0f)) * 180 / M_PI;
                    context->m_handleDPadFunc(angle, TCO_KB_UP);
                }
                break;
            case TOUCHAREA:
                if(context->m_handleTouchFunc){
                    int dx = x - control->m_state.touch_area.m_last_x;
                    int dy = y - control->m_state.touch_area.m_last_y;
                    if (dx != 0 || dy != 0) {
                        context->m_handleTouchFunc(dx, dy);
                        control->m_state.touch_area.m_last_x = x;
                        control->m_state.touch_area.m_last_y = y;
                    }
                }
                break;
            case MOUSEBUTTON:
                if(context->m_handleMouseButtonFunc) {
                    context->m_handleMouseButtonFunc(control->m_properties.mouse.m_button,
                                                     control->m_properties.mouse.m_mask,
                                                     TCO_MOUSE_BUTTON_UP);
                }
                break;
            case TOUCHSCREEN:
                control->m_state.touch_screen.m_touchScreenInHoldEvent = false;
                control->m_state.touch_screen.m_touchScreenInMoveEvent = false;
                break;
            default:
                break;
            }
            control->m_touchId = -1;
            return false;
        }

        /* We have had a previous touch point from this contact and this point is in bounds */
        switch (control->m_type)
        {
        case KEY:
            if (type == SCREEN_EVENT_MTOUCH_RELEASE)
            {
                if(context->m_handleKeyFunc) {
                    context->m_handleKeyFunc(control->m_properties.key.m_symbol,
                                             control->m_properties.key.m_modifier,
                                             control->m_properties.key.m_scancode,
                                             control->m_properties.key.m_unicode,
                                             TCO_KB_UP);
                }
            }
            break;
        case DPAD:
            if(context->m_handleDPadFunc) {
                int angle = atan2((y - control->m_y - control->m_height / 2.0f),
                                  (x - control->m_x - control->m_width / 2.0f)) * 180 / M_PI;
                int event = type == SCREEN_EVENT_MTOUCH_RELEASE ? TCO_KB_UP : TCO_KB_DOWN;
                context->m_handleDPadFunc(angle, event);
            }
            break;
        case TOUCHAREA:
            if(context->m_handleTouchFunc){
                if (type == SCREEN_EVENT_MTOUCH_RELEASE &&
                    (timestamp - control->m_state.touch_area.m_touchDownTime) < TAP_THRESHOLD) {
                    context->m_handleTapFunc();
                } else {
                    if (type == SCREEN_EVENT_MTOUCH_TOUCH) {
                        control->m_state.touch_area.m_touchDownTime = timestamp;
                    }
                    int dx = x - control->m_state.touch_area.m_last_x;
                    int dy = y - control->m_state.touch_area.m_last_y;
                    if (dx != 0 || dy != 0) {
                        context->m_handleTouchFunc(dx, dy);
                        control->m_state.touch_area.m_last_x = x;
                        control->m_state.touch_area.m_last_y = y;
                    }
                }
            }
            break;
        case MOUSEBUTTON:
            if (type == SCREEN_EVENT_MTOUCH_RELEASE)
            {
                if(context->m_handleMouseButtonFunc) {
                    context->m_handleMouseButtonFunc(control->m_properties.mouse.m_button,
                                                     control->m_properties.mouse.m_mask,
                                                     TCO_MOUSE_BUTTON_UP);
                }
            }
            break;
        case TOUCHSCREEN:
            if(context->m_handleTouchScreenFunc) {
                int distance = abs(x - control->m_state.touch_screen.m_start_x) +
                               abs(y - control->m_state.touch_screen.m_start_y);
                if (!control->m_state.touch_screen.m_touchScreenInHoldEvent) {
                    if ((type == SCREEN_EVENT_MTOUCH_RELEASE) &&
                        (timestamp - control->m_state.touch_screen.m_touchScreenStartTime) < TAP_THRESHOLD &&
                        distance < JITTER_THRESHOLD) {
                        context->m_handleTouchScreenFunc(x, y, 1, 0);
                    } else if ((type == SCREEN_EVENT_MTOUCH_MOVE) &&
                               (control->m_state.touch_screen.m_touchScreenInMoveEvent || (distance > JITTER_THRESHOLD))) {
                        control->m_state.touch_screen.m_touchScreenInMoveEvent = true;
                        context->m_handleTouchScreenFunc(x, y, 0, 0);
                    } else if ((type == SCREEN_EVENT_MTOUCH_MOVE) &&
                               (!control->m_state.touch_screen.m_touchScreenInMoveEvent) &&
                               (timestamp - control->m_state.touch_screen.m_touchScreenStartTime) > 2*TAP_THRESHOLD) {
                        control->m_state.touch_screen.m_touchScreenInHoldEvent = true;
                        context->m_handleTouchScreenFunc(x, y, 0, 1);
                    }
                }
            }
            break;
        default:
            break;
        }

        if (type == SCREEN_EVENT_MTOUCH_RELEASE) {
            control->m_touchId = -1;
            control->m_state.touch_screen.m_touchScreenInHoldEvent = false;
            control->m_state.touch_screen.m_touchScreenInMoveEvent = false;
            return false;
        }
    }

    return true;
}

/* TCO context functions */
static tco_control_t tco_context_control_at(tco_context_t ctx,
                                            int x,
                                            int y);

static tco_context_t tco_context_alloc(screen_context_t screenContext,
                                       struct tco_callbacks callbacks)
{
    int rc = bps_initialize();
    if(rc != BPS_SUCCESS) {
        DEBUGLOG("bps: %s (%d)", strerror(errno), errno);
        return NULL;
    }

    tco_context_t ctx = (tco_context_t) calloc(1, sizeof(struct tco_context));
    if(ctx) {
        ctx->m_screenContext = screenContext;
        ctx->m_controls = (tco_control_t*) calloc(MAX_TCO_CONTROLS, sizeof(tco_control_t));
        ctx->m_handleKeyFunc = callbacks.handleKeyFunc;
        ctx->m_handleDPadFunc = callbacks.handleDPadFunc;
        ctx->m_handleMouseButtonFunc = callbacks.handleMouseButtonFunc;
        ctx->m_handleTapFunc = callbacks.handleTapFunc;
        ctx->m_handleTouchFunc = callbacks.handleTouchFunc;
        ctx->m_handleTouchScreenFunc = callbacks.handleTouchScreenFunc;
        SLIST_INIT(&ctx->m_touch_owners);
    } else {
        bps_shutdown();
    }
    return ctx;
}

static void tco_context_free(tco_context_t ctx) {
    if(!ctx) {
        return;
    }

    int i;
    tco_configuration_window_free(ctx->m_configWindow);
    for (i = 0; i < ctx->m_numControls; ++i)
    {
        tco_control_free(ctx->m_controls[i]);
    }
    free(ctx->m_controls);
    ctx->m_controls = NULL;
    ctx->m_numControls = 0;

    touch_owner_t p;
    while(!SLIST_EMPTY(&ctx->m_touch_owners)) {
        p = SLIST_FIRST(&ctx->m_touch_owners);
        SLIST_REMOVE_HEAD(&ctx->m_touch_owners, link);
        free(p);
    }

    free(ctx->m_user_control_path);
    free(ctx);

    bps_shutdown();
}

static tco_control_t tco_context_create_control(tco_context_t ctx,
                                                int id,
                                                const char * controlType,
                                                int x,
                                                int y,
                                                int width,
                                                int height) {
    if(ctx->m_numControls >= MAX_TCO_CONTROLS) {
        DEBUGLOG("Too many controls defined");
        return NULL;
    }
    tco_control_t control = tco_control_alloc(ctx,
                                              id,
                                              controlType,
                                              x,
                                              y,
                                              width,
                                              height);
    ctx->m_controls[ctx->m_numControls] = control;
    ctx->m_numControls++;
    return control;
}

static int tco_context_load_controls(tco_context_t ctx,
                                     const char * default_fileName,
                                     const char * user_fileName) {
    if(!ctx) {
        return TCO_FAILURE;
    }

    int retCode = TCO_FAILURE;
    int version;
    cJSON *root = 0;
    free(ctx->m_user_control_path);
    ctx->m_user_control_path = NULL;
    if(user_fileName) {
        ctx->m_user_control_path = strdup(user_fileName);
    }

    /* Read the user JSON file if it is there */
    char * json_text = tco_read_text_file(user_fileName);
    if(json_text == NULL) {
        /* Read the default JSON file */
        json_text = tco_read_text_file(default_fileName);
    }

    while (json_text)
    {
        /* Parse JSON string */
        root = cJSON_Parse(json_text);
        if (!root)
        {
	        DEBUGLOG("Could not parse JSON from string");
            break;
        }

        /* Check version*/
        version = tco_json_get_int(root, "version");
        if (version != TCO_FILE_VERSION)
        {
            DEBUGLOG("Invalid file version: %d", version);
            break;
        }

        /* Get control descriptions */
        cJSON *controls = cJSON_GetObjectItem(root, "controls");
        if (controls != NULL && controls->type == cJSON_Array)
        {
            int numControls = cJSON_GetArraySize(controls);
            int i;
            for (i = 0; i < numControls; i++)
            {
                cJSON *control = cJSON_GetArrayItem(controls, i);
                if (control != NULL && control->type == cJSON_Object)
                {
                    int id = tco_json_get_int(control, "id");
                    const char * control_type = tco_json_get_str(control, "type");
                    int x = tco_json_get_int(control, "x");
                    int y = tco_json_get_int(control, "y");
                    int width = tco_json_get_int(control, "width");
                    int height = tco_json_get_int(control, "height");

                    tco_control_t c = tco_context_create_control(ctx,
                                                                 id,
                                                                 control_type,
                                                                 x,
                                                                 y,
                                                                 width,
                                                                 height);

                    /* Control specific properties */
                    switch(c->m_type){
                    case KEY:
                        c->m_properties.key.m_symbol = tco_json_get_int(control, "symbol");
                        c->m_properties.key.m_modifier = tco_json_get_int(control, "modifier");
                        c->m_properties.key.m_scancode = tco_json_get_int(control, "scancode");
                        c->m_properties.key.m_unicode = tco_json_get_int(control, "unicode");
                        break;
                    case TOUCHAREA:
                        c->m_properties.touch.m_tapSensitive = tco_json_get_int(control, "tapSensitive");
                        break;
                    case MOUSEBUTTON:
                        c->m_properties.mouse.m_mask = tco_json_get_int(control, "mask");
                        c->m_properties.mouse.m_button = tco_json_get_int(control, "button");
                        break;
                    default:
                        break;
                    }
                    /* Label for the control */
                    cJSON *label = cJSON_GetObjectItem(control, "label");
                    if (label != NULL && label->type == cJSON_Object)
                    {
                        int label_x = tco_json_get_int(label, "x");
                        int label_y = tco_json_get_int(label, "y");
                        int label_width = tco_json_get_int(label, "width");
                        int label_height = tco_json_get_int(label, "height");
                        int label_alpha = tco_json_get_int(label, "alpha");
                        const char * label_image = tco_json_get_str(label, "image");

                        tco_label_t p = tco_label_alloc(ctx,
                                                        c,
                                                        label_x,
                                                        label_y,
                                                        label_width,
                                                        label_height,
                                                        label_alpha,
                                                        label_image);
                        c->m_label = p;
                        p->m_control = c;
                    }
                }
                else
                {
                    DEBUGLOG("Invalid control description");
                    break;
                }
            }
        } else
        {
            DEBUGLOG("Invalid file contents");
            break;
        }

        /* Done */
        retCode = TCO_SUCCESS;
        break;
    }

    if (root != 0)
    {
        /* Delete JSON structure */
        cJSON_Delete(root);
    }

    if (!json_text) {
        DEBUGLOG("Failed to read JSON file");
    }

    free(json_text);
    return retCode;
}

static int tco_context_save_controls(tco_context_t ctx,
                                     const char * user_fileName) {
    if(!ctx) {
        return TCO_FAILURE;
    }
    const char * filePath = user_fileName;
    if(filePath == NULL) {
        filePath = ctx->m_user_control_path;
    }
    if(!filePath) {
        return TCO_SUCCESS; /* No file to be saved, fine */
    }

    cJSON * root = cJSON_CreateObject();
    if(root) {
        tco_json_set_int(root, "version", TCO_FILE_VERSION);
        cJSON * controls_array = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "controls", controls_array);

        int i = 0;
        for(i = 0; i < ctx->m_numControls; ++i) {
            tco_control_t control = ctx->m_controls[i];
            if(!control) {
                continue;
            }

            cJSON * json_control = cJSON_CreateObject();
            cJSON_AddItemToArray(controls_array, json_control);

            switch(control->m_type) {
            case KEY:
                tco_json_set_str(json_control, "type", "key");
                tco_json_set_int(json_control, "symbol", control->m_properties.key.m_symbol);
                tco_json_set_int(json_control, "modifier", control->m_properties.key.m_modifier);
                tco_json_set_int(json_control, "scancode", control->m_properties.key.m_scancode);
                tco_json_set_int(json_control, "unicode", control->m_properties.key.m_unicode);
                break;
            case DPAD:
                tco_json_set_str(json_control, "type", "dpad");
                break;
            case MOUSEBUTTON:
                tco_json_set_str(json_control, "type", "mousebutton");
                tco_json_set_int(json_control, "button", control->m_properties.mouse.m_button);
                tco_json_set_int(json_control, "mask", control->m_properties.mouse.m_mask);
                break;
            case TOUCHAREA:
                tco_json_set_str(json_control, "type", "toucharea");
                tco_json_set_int(json_control, "tapSensitive", control->m_properties.touch.m_tapSensitive);
                break;
            case TOUCHSCREEN:
                tco_json_set_str(json_control, "type", "touchscreen");
                break;
            default:
                tco_json_set_str(json_control, "type", "unknown");
                break;
            }

            tco_json_set_int(json_control, "id", control->m_id);
            tco_json_set_int(json_control, "x", control->m_x);
            tco_json_set_int(json_control, "y", control->m_y);
            tco_json_set_int(json_control, "width", control->m_width);
            tco_json_set_int(json_control, "height", control->m_height);

            if(control->m_label != NULL) {
                cJSON * label = cJSON_CreateObject();
                cJSON_AddItemToObject(json_control, "label", label);

                tco_json_set_int(label, "x", control->m_label->m_x);
                tco_json_set_int(label, "y", control->m_label->m_y);
                tco_json_set_int(label, "width", control->m_label->m_width);
                tco_json_set_int(label, "height", control->m_label->m_height);
                tco_json_set_int(label, "alpha", control->m_label->m_label_window->m_baseWindow.m_alpha);
                if(control->m_label->m_image_file) {
                    tco_json_set_str(label, "image", control->m_label->m_image_file);
                }
            }
        }

        char * json_text = cJSON_Print(root);
        if(json_text) {
            FILE * file = fopen(filePath, "w");
            if(file) {
                size_t nBytes = strlen(json_text);
                int result = fwrite(json_text, nBytes, 1, file);
                if(1 != result) {
                    DEBUGLOG("Failed to save user controls file: %s (%d)", strerror(errno), errno);
                } else {
                    DEBUGLOG("User controls file was saved successfully");
                }
                fclose(file);
            } else {
                DEBUGLOG("Failed to open user controls for writing: %s (%d)", strerror(errno), errno);
            }
            free(json_text);
        } else {
            DEBUGLOG("Failed to create JSON: %s (%d)", strerror(errno), errno);
        }

        cJSON_Delete(root);
    } else {
        return TCO_FAILURE;
    }
    return TCO_SUCCESS;
}

static bool tco_context_touch_event(tco_context_t ctx,
                                    screen_event_t event) {
    if(!ctx) {
        return false;
    }
    int rc;
    int type;
    int touch_id;
    int pos[2];
    int screenPos[2];
    int orientation;
    long long timestamp;
    int sequenceId;
    bool handled = false;

    rc = screen_get_event_property_iv(event, SCREEN_PROPERTY_TYPE, &type);
    if(rc) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }

    rc = screen_get_event_property_iv(event, SCREEN_PROPERTY_TOUCH_ID, &touch_id);
    if(rc) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }

    rc = screen_get_event_property_iv(event, SCREEN_PROPERTY_SOURCE_POSITION, pos);
    if(rc) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }

    rc = screen_get_event_property_iv(event, SCREEN_PROPERTY_POSITION, screenPos);
    if(rc) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }

    rc = screen_get_event_property_iv(event, SCREEN_PROPERTY_TOUCH_ORIENTATION, &orientation);
    if(rc) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }

    rc = screen_get_event_property_llv(event, SCREEN_PROPERTY_TIMESTAMP, &timestamp);
    if(rc) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }

    rc = screen_get_event_property_iv(event, SCREEN_PROPERTY_SEQUENCE_ID, &sequenceId);
    if(rc) {
        DEBUGLOG("screen: %s (%d)", strerror(errno), errno);
        return false;
    }

    /* Find the first owner of the touch_id */
    tco_control_t touchPointOwner = 0;
    touch_owner_t p = NULL;
    SLIST_FOREACH(p, &ctx->m_touch_owners, link) {
        if(p->touch_id == touch_id) {
            touchPointOwner = p->control;
            break;
        }
    }
    if (touchPointOwner) {
        /* touchPointOwner points to control and p points to list entry */
        handled = tco_control_handle_touch(touchPointOwner,
                                           ctx,
                                           type,
                                           touch_id,
                                           pos[0],
                                           pos[1],
                                           timestamp);
        if (!handled) {
            SLIST_REMOVE(&ctx->m_touch_owners, p, touch_owner, link);
            free(p);
        }
    }

    if (!handled) {
        int i;
        for (i = 0; i < ctx->m_numControls; ++i) {
            if (ctx->m_controls[i] == touchPointOwner) {
                continue; /* already checked */
            }

            handled |= tco_control_handle_touch(ctx->m_controls[i],
                                                ctx,
                                                type,
                                                touch_id,
                                                pos[0],
                                                pos[1],
                                                timestamp);
            if (handled) {
                p = (touch_owner_t)calloc(1, sizeof(struct touch_owner));
                p->control = ctx->m_controls[i];
                p->touch_id = touch_id;
                SLIST_INSERT_HEAD(&ctx->m_touch_owners, p, link);
                /* Only allow the first control to handle the touch. */
                break;
            }
        }
    }

    return handled;
}

static int tco_context_handle_events(tco_context_t ctx,
                                     screen_window_t window,
                                     bps_event_t * event) {
    if(!ctx) {
        return TCO_FAILURE;
    }

    int domain;
    int event_code;

    if(ctx->m_configWindow != NULL) {
        /* Configuration window is shown */
        if(!event) {
            tco_configuration_window_run(ctx->m_configWindow, NULL);
            return TCO_SUCCESS;
        }
        domain = bps_event_get_domain(event);
        if (domain == navigator_get_domain()) {
            /* Handle Navigator events*/
            event_code = bps_event_get_code(event);
            switch(event_code) {
            case NAVIGATOR_EXIT:
                tco_configuration_window_free(ctx->m_configWindow);
                ctx->m_configWindow = 0;
                break;
            case NAVIGATOR_SWIPE_DOWN:
                {
                    tco_configuration_window_free(ctx->m_configWindow);
                    ctx->m_configWindow = 0;
                    tco_context_save_controls(ctx, ctx->m_user_control_path);
                }
                return TCO_SUCCESS;
            default:
                break;
            }
        } else if (domain == screen_get_domain()) {
            int event_type; /* event type */
            screen_event_t screen_event = screen_event_get_event(event);
            int rc = screen_get_event_property_iv(screen_event,
                                                  SCREEN_PROPERTY_TYPE,
                                                  &event_type);
            if (rc == 0) {
                switch(event_type) {
                case SCREEN_EVENT_CLOSE:
                    tco_configuration_window_free(ctx->m_configWindow);
                    ctx->m_configWindow = 0;
                    break;
                case SCREEN_EVENT_MTOUCH_TOUCH:
                case SCREEN_EVENT_MTOUCH_MOVE:
                case SCREEN_EVENT_MTOUCH_RELEASE:
                    return tco_configuration_window_run(ctx->m_configWindow, screen_event);
                default:
                    break;
                }
            }
        }
    } else {
        if(!event) {
            return TCO_SUCCESS;
        }
        /* Configuration window is not shown */
        domain = bps_event_get_domain(event);
        if (domain == navigator_get_domain()) {
            /* Handle Navigator events*/
            event_code = bps_event_get_code(event);
            switch(event_code) {
            case NAVIGATOR_EXIT:
                break;
            case NAVIGATOR_SWIPE_DOWN:
                /* Start configuration window */
                ctx->m_configWindow = tco_configuration_alloc(ctx, window);
                if (ctx->m_configWindow) {
                    return TCO_SUCCESS;
                } else {
                    return TCO_FAILURE;
                }
            default:
                break;
            }
        } else if (domain == screen_get_domain()) {
            int event_type; /* event type */
            screen_event_t screen_event = screen_event_get_event(event);
            int rc = screen_get_event_property_iv(screen_event,
                                                  SCREEN_PROPERTY_TYPE,
                                                  &event_type);
            if (rc == 0) {
                switch(event_type) {
                case SCREEN_EVENT_CLOSE:
                    break;
                case SCREEN_EVENT_MTOUCH_TOUCH:
                case SCREEN_EVENT_MTOUCH_MOVE:
                case SCREEN_EVENT_MTOUCH_RELEASE:
                    return tco_context_touch_event(ctx, screen_event) ? TCO_SUCCESS : TCO_UNHANDLED;
                default:
                    break;
                }
            }
        }
    }
    return TCO_UNHANDLED;
}

static int tco_context_draw(tco_context_t ctx,
                            screen_window_t window) {
    if(!ctx) {
        return TCO_FAILURE;
    }
    int i;
    for (i = 0; i < ctx->m_numControls; ++i)
    {
        if(!tco_control_draw_label(ctx->m_controls[i], window)) {
            return TCO_FAILURE;
        }
    }
    return TCO_SUCCESS;
}

static tco_control_t tco_context_control_at(tco_context_t ctx,
                                            int x,
                                            int y) {
    if(!ctx) {
        return NULL;
    }
    int i;
    for (i = 0; i < ctx->m_numControls; ++i)
    {
        if (tco_control_in_bounds(ctx->m_controls[i], x, y))
        {
            return ctx->m_controls[i];
        }
    }
    return NULL;
}

/* Public TCO API functions */
int tco_initialize(tco_context_t *context,
                   screen_context_t screenContext,
                   struct tco_callbacks callbacks) {
    *context = tco_context_alloc(screenContext, callbacks);
    return *context ? TCO_SUCCESS : TCO_FAILURE;
}

int tco_loadcontrols(tco_context_t context,
                     const char* default_filename,
                     const char* user_filename) {
    tco_context_t c = (tco_context_t)context;
    return tco_context_load_controls(c,
                                     default_filename,
                                     user_filename);
}

int tco_savecontrols(tco_context_t context,
                     const char* user_filename) {
    tco_context_t c = (tco_context_t)context;
    return tco_context_save_controls(c,
                                     user_filename);
}

int tco_handle_events(tco_context_t context,
                      screen_window_t window,
                      bps_event_t * event) {
    if(!event) {
        return TCO_SUCCESS;
    }
    tco_context_t c = (tco_context_t)context;
    return tco_context_handle_events(c, window, event);
}

int tco_draw(tco_context_t context,
             screen_window_t window) {
    tco_context_t c = (tco_context_t)context;
    return tco_context_draw(c, window);
}

void tco_shutdown(tco_context_t context) {
    tco_context_t c = (tco_context_t)context;
    tco_context_free(c);
}
