#include "stdlib.h"
#include "string.h"
#include "stdbool.h"
#include <png.h>
#include <bps/bps.h>
#include <bps/screen.h>
#include <bps/event.h>
#include <bps/navigator.h>
#include "cJSON.h"
#include "tco.h"

#define MAX_TCO_CONTROLS 8

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

/* TCO window */
struct tco_window {
    screen_context_t m_context;
    screen_window_t m_window;
    screen_window_t m_parent;
    int m_size[2];
    int m_alpha;
};

/* TCO label window */
struct tco_label_window {
    struct tco_window m_baseWindow; /* Must be first member */
    int m_offset[2];
    float m_scale[2];
};

/* TCO configuration window */
struct tco_configuration_window {
    struct tco_window m_baseWindow; /* Must be first member */
    tco_control_t m_selected;
};

/* TCO label */
struct tco_label {
    int m_x;
    int m_y;
    int m_width;
    int m_height;
    tco_control_t m_control;
    tco_label_window_t m_window;
};

/* TCO control */
struct tco_control {
    tco_control_type m_type;

    int m_id;

    int m_x;
    int m_y;
    int m_width;
    int m_height;
    int m_srcWidth;
    int m_srcHeight;

    //EventDispatcher *m_dispatcher;
    //EventDispatcher *m_tapDispatcher;

    screen_context_t m_context;
    screen_pixmap_t m_pixmap;
    screen_buffer_t m_buffer;

    int m_contactId;

    /* For touch areas */
    int m_lastPos[2];
    long long m_touchDownTime;

    /* For touch screens */
    int m_startPos[2];
    long long m_touchScreenStartTime;
    bool m_touchScreenInMoveEvent;
    bool m_touchScreenInHoldEvent;

    tco_label_t m_label;

    union {
        struct {
            int m_symbol;
            int m_modifier;
            int m_scancode;
            int m_unicode;
        } key;
        struct {
            int m_mask;
            int m_button;
        } mouse;
        struct {
            int m_tapSensitive;
        } touch;

    } m_properties;
};

/* TCO context */
struct tco_context {
    screen_context_t m_screenContext;
    screen_window_t m_appWindow;
    tco_configuration_window_t m_configWindow;

    tco_control_t * m_controls;
    int m_numControls;
    //std::map<int, Control *> m_controlMap;

    HandleKeyFunc m_handleKeyFunc;
    HandleDPadFunc m_handleDPadFunc;
    HandleTouchFunc m_handleTouchFunc;
    HandleMouseButtonFunc m_handleMouseButtonFunc;
    HandleTapFunc m_handleTapFunc;
    HandleTouchScreenFunc m_handleTouchScreenFunc;
};

/* TCO window functions */
static bool tco_window_set_parent(tco_window_t window,
                                  screen_window_t parent);

static void tco_window_init(tco_window_t window,
                            int width,
                            int height,
                            int alpha,
                            screen_window_t parent);

static bool tco_window_set_z_order(tco_window_t window,
                                   int zOrder);

static bool tco_window_set_touch_sensitivity(tco_window_t window,
                                             int sensitivity);

static void tco_window_post(tco_window_t window,
                            screen_buffer_t buffer);

static void tco_window_done(tco_window_t window);

static bool tco_window_set_parent(tco_window_t window,
                                  screen_window_t parent) {
    if(!window)
        return false;
    int rc;
    if (parent == window->m_parent) {
        return true;
    }

    if (parent != 0) {
        char buffer[256] = {0};

        rc = screen_get_window_property_cv(parent, SCREEN_PROPERTY_GROUP, 256, buffer);
        if (rc) {
#ifdef _DEBUG
            perror("screen_get_window_property_cv(SCREEN_PROPERTY_GROUP)");
#endif
            return false;
        }

        rc = screen_join_window_group(window->m_window, buffer);
        if (rc) {
#ifdef _DEBUG
            perror("screen_join_window_group");
#endif
            return false;
        }
        window->m_parent = parent;
    } else if (window->m_parent) {
        rc = screen_leave_window_group(window->m_window);
        if (rc) {
#ifdef _DEBUG
            perror("screen_leave_window_group");
#endif
            return false;
        }
        window->m_parent = 0;
    }
    return true;
}

static void tco_window_init(tco_window_t window,
                            int width,
                            int height,
                            int alpha,
                            screen_window_t parent) {
    if(!window)
        return;
    int rc;
    int format = SCREEN_FORMAT_RGBA8888;
    int usage = SCREEN_USAGE_NATIVE | SCREEN_USAGE_READ | SCREEN_USAGE_WRITE;
    int temp[2];

    memset(window, 0, sizeof(struct tco_window));
    window->m_size[0] = width;
    window->m_size[1] = height;
    window->m_alpha = alpha;

    rc = screen_create_window_type(&window->m_window,
                                    window->m_context,
                                    SCREEN_CHILD_WINDOW);
    if (rc) {
#ifdef _DEBUG
        perror("screen_create_window");
#endif
        return;
    }

    rc = screen_set_window_property_iv(window->m_window,
                                       SCREEN_PROPERTY_FORMAT,
                                       &format);
    if (rc) {
#ifdef _DEBUG
        perror("screen_set_window_property_iv(SCREEN_PROPERTY_FORMAT)");
#endif
        screen_destroy_window(window->m_window);
        window->m_window = 0;
        return;
    }

    rc = screen_set_window_property_iv(window->m_window,
                                       SCREEN_PROPERTY_USAGE,
                                       &usage);
    if (rc) {
#ifdef _DEBUG
        perror("screen_set_window_property_iv(SCREEN_PROPERTY_USAGE)");
#endif
        screen_destroy_window(window->m_window);
        window->m_window = 0;
        return;
    }

    if (parent) {
        rc = screen_set_window_property_iv(parent,
                                           SCREEN_PROPERTY_SIZE,
                                           temp);
        if (rc) {
#ifdef _DEBUG
            perror("screen_set_window_property_iv(SCREEN_PROPERTY_SIZE)");
#endif
            screen_destroy_window(window->m_window);
            window->m_window = 0;
            return;
        }

        rc = screen_set_window_property_iv(window->m_window,
                                           SCREEN_PROPERTY_SIZE,
                                           temp);
        if (rc) {
#ifdef _DEBUG
            perror("screen_set_window_property_iv(SCREEN_PROPERTY_SIZE)");
#endif
            screen_destroy_window(window->m_window);
            window->m_window = 0;
            return;
        }

        rc = screen_get_window_property_iv(parent,
                                           SCREEN_PROPERTY_POSITION,
                                           temp);
        if (rc) {
#ifdef _DEBUG
            perror("screen_get_window_property_iv(SCREEN_PROPERTY_POSITION)");
#endif
            screen_destroy_window(window->m_window);
            window->m_window = 0;
            return;
        }

        rc = screen_set_window_property_iv(window->m_window,
                                           SCREEN_PROPERTY_POSITION,
                                           temp);
        if (rc) {
#ifdef _DEBUG
            perror("screen_set_window_property_iv(SCREEN_PROPERTY_POSITION)");
#endif
            screen_destroy_window(window->m_window);
            window->m_window = 0;
            return;
        }
    } else {
        rc = screen_set_window_property_iv(window->m_window,
                                           SCREEN_PROPERTY_SIZE,
                                           window->m_size);
        if (rc) {
#ifdef _DEBUG
            perror("screen_set_window_property_iv(SCREEN_PROPERTY_SIZE)");
#endif
            screen_destroy_window(window->m_window);
            window->m_window = 0;
            return;
        }
    }

    rc = screen_set_window_property_iv(window->m_window,
                                       SCREEN_PROPERTY_BUFFER_SIZE,
                                       window->m_size);
    if (rc) {
#ifdef _DEBUG
        perror("screen_set_window_property_iv(SCREEN_PROPERTY_BUFFER_SIZE)");
#endif
        screen_destroy_window(window->m_window);
        window->m_window = 0;
        return;
    }

    rc = screen_create_window_buffers(window->m_window, 2);
    if (rc) {
#ifdef _DEBUG
        perror("screen_create_window_buffers");
#endif
        screen_destroy_window(window->m_window);
        window->m_window = 0;
        return;
    }

    if (!tco_window_set_parent(window, parent))
    {
        screen_destroy_window(window->m_window);
        window->m_window = 0;
        return;
    }
}

static bool tco_window_set_z_order(tco_window_t window,
                                   int zOrder) {
    if(!window)
        return false;
    int rc = screen_set_window_property_iv(window->m_window,
                                           SCREEN_PROPERTY_ZORDER,
                                           &zOrder);
    if (rc) {
#ifdef _DEBUG
        fprintf(stderr, "Cannot set z-order: %s", strerror(errno));
#endif
        return false;
    }
    return true;
}

static bool tco_window_set_touch_sensitivity(tco_window_t window,
                                             int sensitivity) {
    if(!window)
        return false;
    sensitivity = (sensitivity > 0 ? SCREEN_SENSITIVITY_ALWAYS:SCREEN_SENSITIVITY_NEVER);
    int rc = screen_set_window_property_iv(window->m_window,
                                           SCREEN_PROPERTY_SENSITIVITY,
                                           &sensitivity);
    if (rc) {
#ifdef _DEBUG
        fprintf(stderr, "Cannot set screen sensitivity: %s", strerror(errno));
#endif
        return false;
    }
    return true;
}

static void tco_window_post(tco_window_t window,
                            screen_buffer_t buffer) {
    if(!window)
        return;
    int dirtyRects[4] = {0, 0, window->m_size[0], window->m_size[1]};
    screen_post_window(window->m_window, buffer, 1, dirtyRects, 0);
}

static void tco_window_done(tco_window_t window) {
    if(!window)
        return;
    if (window->m_window) {
        screen_destroy_window(window->m_window);
        window->m_window = 0;
    }
    memset(window, 0, sizeof(struct tco_window));
}


/* TCO label window functions */
static tco_label_window_t tco_label_window_alloc(screen_context_t context,
                                                 int width,
                                                 int height,
                                                 int alpha);

static void tco_label_window_free(tco_label_window_t window);

static void tco_label_window_show_at(tco_label_window_t window,
                                     screen_window_t parent,
                                     int x,
                                     int y);

static void tco_label_window_move(tco_label_window_t window,
                                  int x,
                                  int y);

static void tco_label_window_draw(tco_label_window_t label_window,
                                  screen_window_t window,
                                  int x,
                                  int y);

static tco_label_window_t tco_label_window_alloc(screen_context_t context,
                                                 int width,
                                                 int height,
                                                 int alpha) {
    tco_label_window_t window = (tco_label_window_t)calloc(1, sizeof(struct tco_label_window));
    tco_window_init(&window->m_baseWindow, width, height, alpha, 0);

    tco_window_set_z_order(&window->m_baseWindow, 6);
    tco_window_set_touch_sensitivity(&window->m_baseWindow, 0);

    window->m_offset[0] = 0;
    window->m_offset[1] = 0;
    window->m_scale[0] = 1.0f;
    window->m_scale[1] = 1.0f;
    return window;
}

static void tco_label_window_free(tco_label_window_t window) {
    if(window) {
        //TODO:
        tco_window_done(&window->m_baseWindow);
        free(window);
    }
}

static void tco_label_window_show_at(tco_label_window_t window,
                                     screen_window_t parent,
                                     int x,
                                     int y) {
    if(!window)
        return;
    int rc = 0;
    if (parent && parent != window->m_baseWindow.m_parent) {
        int parentBufferSize[2];
        int parentSize[2];
        rc = screen_get_window_property_iv(parent, SCREEN_PROPERTY_POSITION, window->m_offset);
        rc = screen_get_window_property_iv(parent, SCREEN_PROPERTY_BUFFER_SIZE, parentBufferSize);
        rc = screen_get_window_property_iv(parent, SCREEN_PROPERTY_SIZE, parentSize);
        window->m_scale[0] = parentSize[0] / (float)parentBufferSize[0];
        window->m_scale[1] = parentSize[1] / (float)parentBufferSize[1];
        int newSize[] = {window->m_baseWindow.m_size[0] * window->m_scale[0],
                         window->m_baseWindow.m_size[1] * window->m_scale[1]};
        rc = screen_set_window_property_iv(window->m_baseWindow.m_window,
                                           SCREEN_PROPERTY_SIZE,
                                           newSize);
    }

    if (!tco_window_set_parent(&window->m_baseWindow, parent))
        return;

    tco_label_window_move(window, x, y);

    int visible = 1;
    rc = screen_set_window_property_iv(window->m_baseWindow.m_window,
                                       SCREEN_PROPERTY_VISIBLE,
                                       &visible);
#ifdef _DEBUG
    if (rc) {
        perror("set label window visible: ");
    }
#endif
}

static void tco_label_window_move(tco_label_window_t window,
                                  int x,
                                  int y) {
    if(!window)
        return;
    int position[] = {window->m_offset[0] + (x * window->m_scale[0]),
                      window->m_offset[1] + (y * window->m_scale[1])};
    int rc = screen_set_window_property_iv(window->m_baseWindow.m_window,
                                           SCREEN_PROPERTY_POSITION,
                                           position);
#ifdef _DEBUG
    if (rc) {
        perror("screen_set_window_property_iv");
        return;
    }
#endif
}

static void tco_label_window_draw(tco_label_window_t label_window,
                                  screen_window_t window,
                                  int x,
                                  int y) {
    if(!label_window)
        return;
    tco_label_window_show_at(label_window, window, x, y);
}


/* Configuration window functions */
static tco_configuration_window_t tco_configuration_alloc();

static void tco_configuration_window_run(tco_configuration_window_t window);

static void tco_configuration_window_free(tco_configuration_window_t window);

static tco_configuration_window_t tco_configuration_alloc() {
    tco_configuration_window_t window = (tco_configuration_window_t)calloc(1, sizeof(struct tco_configuration_window));
    tco_window_init(&window->m_baseWindow, width, height, alpha, 0);

    tco_window_set_z_order(&window->m_baseWindow, 10);
    tco_window_set_touch_sensitivity(&window->m_baseWindow, 1);
    return window;
}

static void tco_configuration_window_run(tco_configuration_window_t window) {
    //TODO:
}

static void tco_configuration_window_free(tco_configuration_window_t window) {
    if (window) {
        tco_window_done(&window->m_baseWindow);
        free(window);
    }
}


/* Label functions */
static tco_label_t tco_label_alloc(screen_context_t context,
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
    label->m_window = tco_label_window_alloc(context,
                                             width,
                                             height,
                                             alpha);
    //TODO: image
    //label->m_window->draw(png);
    return label;
}

static void tco_label_free(tco_label_t label) {
    if(label) {
        label->m_control = NULL;
        tco_label_window_free(label->m_window);
        //TODO: free image?
        free(label);
    }
}

static void tco_label_draw(tco_label_t label,
                           screen_window_t window,
                           int x,
                           int y) {
    tco_label_window_draw(label->m_window,
                          window,
                          label->m_x + x,
                          label->m_y + y);
}

static void tco_label_move(tco_label_t label,
                           int x,
                           int y) {
    tco_label_window_move(label->m_window,
                          label->m_x + x,
                          label->m_y + y);
}


/* Control functions */
static tco_control_t tco_control_alloc(screen_context_t context,
                                       int id,
                                       const char * controlType,
                                       int x,
                                       int y,
                                       int width,
                                       int height);

static void tco_control_free(tco_control_t control);

static void tco_control_add_label(tco_control_t control,
                                  tco_label_t label);

static void tco_control_draw(tco_control_t control,
                             screen_buffer_t buffer);

static void tco_control_move(tco_control_t control,
                             int dx,
                             int dy,
                             int max_x,
                             int max_h);

static void tco_control_fill(tco_control_t control);

static void tco_control_show_label(tco_control_t control,
                                   screen_window_t window);

static bool tco_control_in_bounds(tco_control_t control,
                                  int x,
                                  int y);

static bool tco_control_handle_tap(tco_control_t control,
                                   int contactId,
                                   const int pos[]);

static bool tco_control_handle_touch(tco_control_t control,
                                     int type,
                                     int contactId,
                                     const int pos[],
                                     long long timestamp);

static tco_control_t tco_control_alloc(screen_context_t context,
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
    control->m_contactId = -1;
    return control;
}

static void tco_control_free(tco_control_t control) {
    if(control->m_pixmap!=NULL) {
        screen_destroy_pixmap(control->m_pixmap);
        control->m_pixmap = NULL;
    }
    //TODO:m_buffer
    tco_label_free(control->m_label);
    free(control);
}

static void tco_control_add_label(tco_control_t control, tco_label_t label) {
    control->m_label = label;
    label->m_control = control;
}

static void tco_control_draw(tco_control_t control, screen_buffer_t buffer) {
    screen_get_pixmap_property_pv(control->m_pixmap,
                                  SCREEN_PROPERTY_RENDER_BUFFERS,
                                  (void**)&control->m_buffer);
    int attribs[] = {
            SCREEN_BLIT_SOURCE_X, 0,
            SCREEN_BLIT_SOURCE_Y, 0,
            SCREEN_BLIT_SOURCE_WIDTH, control->m_srcWidth,
            SCREEN_BLIT_SOURCE_HEIGHT, control->m_srcHeight,
            SCREEN_BLIT_DESTINATION_X, control->m_x,
            SCREEN_BLIT_DESTINATION_Y, control->m_y,
            SCREEN_BLIT_DESTINATION_WIDTH, control->m_width,
            SCREEN_BLIT_DESTINATION_HEIGHT, control->m_height,
            SCREEN_BLIT_TRANSPARENCY, SCREEN_TRANSPARENCY_SOURCE_OVER,
            SCREEN_BLIT_END
    };
    screen_blit(control->m_context, buffer, control->m_buffer, attribs);
}

static void tco_control_move(tco_control_t control,
                             int dx,
                             int dy,
                             int max_x,
                             int max_y) {
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
    tco_label_move(control->m_label, control->m_x, control->m_y);
}

static void tco_control_fill(tco_control_t control) {
    static unsigned controlNum = 0;
    static uint32_t controlColors[] = { 0xaaff0000, 0xaa00ff00, 0xaa0000ff, 0xaaffff00, 0xaaff00ff, 0xaa00ffff };

    int format = SCREEN_FORMAT_RGBA8888;
    int size[2] = {control->m_width, control->m_height};

    int rc = screen_create_pixmap(&control->m_pixmap,
                                  control->m_context);
    if (rc) {
#ifdef _DEBUG
        fprintf(stderr, "screen_create_pixmap: %s", strerror(errno));
#endif
        return;
    }

    rc = screen_set_pixmap_property_iv(control->m_pixmap,
                                       SCREEN_PROPERTY_FORMAT,
                                       &format);
    if (rc) {
#ifdef _DEBUG
        fprintf(stderr, "screen_set_pixmap_property_iv: %s", strerror(errno));
#endif
        return;
    }

    rc = screen_set_pixmap_property_iv(control->m_pixmap,
                                       SCREEN_PROPERTY_BUFFER_SIZE,
                                       size);
    if (rc) {
#ifdef _DEBUG
        fprintf(stderr, "screen_set_pixmap_property_iv: %s", strerror(errno));
#endif
        return;
    }

    rc = screen_create_pixmap_buffer(control->m_pixmap);
    if (rc) {
#ifdef _DEBUG
        fprintf(stderr, "screen_create_pixmap_buffer: %s", strerror(errno));
#endif
        return;
    }
    rc = screen_get_pixmap_property_pv(control->m_pixmap,
                                       SCREEN_PROPERTY_RENDER_BUFFERS,
                                       (void**)&control->m_buffer);
    if (rc) {
#ifdef _DEBUG
        fprintf(stderr, "screen_get_pixmap_property_pv: %s", strerror(errno));
#endif
        return;
    }

    int attribs[] = {
        SCREEN_BLIT_COLOR, (int)controlColors[controlNum],
        SCREEN_BLIT_END
    };
    rc = screen_fill(control->m_context, control->m_buffer, attribs);
    if (rc) {
#ifdef _DEBUG
        fprintf(stderr, "screen_fill: %s", strerror(errno));
#endif
        return;
    }
    controlNum++;
    if (controlNum > 5) {
        controlNum = 0;
    }
}

static void tco_control_show_label(tco_control_t control,
                                   screen_window_t window) {
    if(!control)
        return;
    if(control->m_label!=NULL) {
        tco_label_draw(control->m_label, window, control->m_x, control->m_y);
    }
}

static bool tco_control_in_bounds(tco_control_t control,
                                  int x,
                                  int y) {
    if(!control)
        return false;
    return (x >= control->m_x &&
            x <= control->m_x + control->m_width &&
            y >= control->m_y &&
            y <= control->m_y + control->m_height);
}

static bool tco_control_handle_tap(tco_control_t control,
                                   int contactId,
                                   const int pos[]) {
    if(!control)
        return false;
    /*if (!m_tapDispatcher)
        return false;

    if (m_contactId != -1) {
        //  We have a contact point set already. No taps allowed.
        return false;
    }
    if (!inBounds(pos))
        return false;

    return m_tapDispatcher->runCallback(0);*/
    return false;
}

static bool tco_control_handle_touch(tco_control_t control,
                                     int type,
                                     int contactId,
                                     const int pos[],
                                     long long timestamp) {
    if(!control)
        return false;
    return false;
}

static void tco_control_load_image(tco_control_t control, const char * imageFile) {
    if(!control)
        return;
    //TODO:
//FILE *file = fopen(filename, "rb");
//    if (!file) {
//#ifdef _DEBUG
//        fprintf(stderr, "Unable to open file %s\n", filename);
//#endif
//        return false;
//    }
//
//    PNGReader png(file, filename, m_context);
//    if (!png.doRead())
//        return false;
//
//    m_srcWidth = png.m_width;
//    m_srcHeight = png.m_height;
//    m_pixmap = png.m_pixmap;
//    png.m_pixmap = 0;
//    m_buffer = png.m_buffer;
//    png.m_buffer = 0;
}

static char * tco_read_file(const char * fileName)
{
    long len;
    char *buf = 0;
    FILE *fp = fopen(fileName, "r");
    while (fp)
    {
        if (0 != fseek(fp, 0, SEEK_END))
        {
            break;
        }
        len = ftell(fp);
        if (len == -1)
        {
            break;
        }
        if (0 != fseek(fp, 0, SEEK_SET))
        {
            break;
        }
        buf = (char *) calloc(1, len + 1);
        if (!buf)
        {
            break;
        }
        fread(buf, len, 1, fp);
        if (ferror(fp))
        {
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

static int tco_json_get_int(cJSON * object, const char * name)
{
    cJSON * value = cJSON_GetObjectItem(object, name);
    if (value != NULL && value->type == cJSON_Number)
    {
        return value->valueint;
    }
    return 0;
}

static const char * tco_json_get_str(cJSON * object, const char * name)
{
    cJSON * value = cJSON_GetObjectItem(object, name);
    if (value != NULL && value->type == cJSON_String)
    {
        return value->valuestring;
    }
    return 0;
}

/* TCO context functions */
static tco_context_t tco_context_alloc(screen_context_t screenContext,
                                       struct tco_callbacks callbacks);

static void tco_context_free(tco_context_t ctx);

static tco_control_t tco_context_create_control(tco_context_t ctx,
                                                int id,
                                                const char * controlType,
                                                int x,
                                                int h,
                                                int width,
                                                int height);

static int tco_context_load_controls(tco_context_t ctx,
                                     const char * fileName);

static int tco_context_load_default_controls(tco_context_t ctx);

static void tco_context_draw_controls(tco_context_t ctx,
                                      screen_buffer_t buffer);

static int tco_context_show_configuration(tco_context_t ctx,
                                          screen_window_t window);

static int tco_context_show_labels(tco_context_t ctx,
                                   screen_window_t window);

static tco_control_t tco_context_control_at(tco_context_t ctx,
                                            int x,
                                            int y);

static tco_context_t tco_context_alloc(screen_context_t screenContext,
                                       struct tco_callbacks callbacks)
{
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
    //TODO:
    //std::map<int, Control *> m_controlMap;
    free(ctx);
}

static tco_control_t tco_context_create_control(tco_context_t ctx,
                                                int id,
                                                const char * controlType,
                                                int x,
                                                int y,
                                                int width,
                                                int height) {
    tco_control_t control = tco_control_alloc(ctx->m_screenContext,
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
                                     const char * fileName) {
    int retCode = TCO_FAILURE;
    if(!ctx)
        return retCode;
    /* Read JSON file */
    int version;
    cJSON *root = 0;
    char * s = tco_read_file(fileName);
    while (s)
    {
        /* Parse JSON string */
        root = cJSON_Parse(s);
        if (!root)
        {
            printf("Could not parse JSON from string\n");
            break;
        }

        /* Check version*/
        version = tco_json_get_int(root, "version");
        if (version != TCO_FILE_VERSION)
        {
            printf("Invalid file version: %d\n", version);
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
                    const char * image = tco_json_get_str(control, "image");

                    tco_control_t c = tco_context_create_control(ctx,
                                                                 id,
                                                                 control_type,
                                                                 x,
                                                                 y,
                                                                 width,
                                                                 height);
                    /* Image for the control */
                    if(image) {
                        tco_control_load_image(c, image);
                    } else {
                        tco_control_fill(c);
                    }
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
                        const char * label_image = tco_json_get_str(label, "image");
                        int label_alpha = tco_json_get_int(label, "alpha");

                        tco_label_t p = tco_label_alloc(ctx->m_screenContext,
                                                        c,
                                                        label_x,
                                                        label_y,
                                                        label_width,
                                                        label_height,
                                                        label_alpha,
                                                        label_image);
                        tco_control_add_label(c, p);
                    }
                }
                else
                {
                    printf("Invalid control description\n");
                    break;
                }
            }
        } else
        {
            printf("Invalid file contents\n");
            break;
        }

        /* Done */
        retCode = TCO_SUCCESS;
        break;
    }

    if (root != 0)
    {
        /* Save JSON */
        /*char * contents = cJSON_Print(root);
         if(contents) {
         printf("%s\n", contents);
         free(contents);
         }*/

        /* Delete JSON structure */
        cJSON_Delete(root);
    }

    if (!s) {
        printf("Failed to read JSON file\n");
    }
    free(s);
    return retCode;
}

static int tco_context_load_default_controls(tco_context_t ctx) {
    if(!ctx)
        return TCO_FAILURE;
    int width = atoi(getenv("WIDTH"));
    int height = atoi(getenv("HEIGHT"));
    tco_control_t control = tco_context_create_control(ctx,
                                                       0,
                                                       "touchscreen",
                                                       0,
                                                       0,
                                                       width,
                                                       height);
    tco_control_fill(control);
    return TCO_SUCCESS;
}

static void tco_context_draw_controls(tco_context_t ctx,
                                      screen_buffer_t buffer) {
    if(!ctx)
        return;
    int i;
    for (i = 0; i < ctx->m_numControls; ++i)
    {
        tco_control_draw(ctx->m_controls[i], buffer);
    }
}

static int tco_context_show_configuration(tco_context_t ctx,
                                          screen_window_t window) {
    if(!ctx)
        return TCO_FAILURE;
    ctx->m_appWindow = window;
    if (!ctx->m_configWindow)
    {
        // TODO:
        ctx->m_configWindow = 0; //ConfigWindow::createConfigWindow(m_screenContext, window);
        if (!ctx->m_configWindow)
        {
            return TCO_FAILURE;
        }
        tco_configuration_window_run(ctx->m_configWindow);
        tco_configuration_window_free(ctx->m_configWindow);
        ctx->m_configWindow = 0;
    }
    return TCO_SUCCESS;
}

static int tco_context_show_labels(tco_context_t ctx,
                                   screen_window_t window) {
    if(!ctx)
        return TCO_FAILURE;
    int i;
    for (i = 0; i < ctx->m_numControls; ++i)
    {
        tco_control_show_label(ctx->m_controls[i], window);
    }
    return TCO_SUCCESS;
}

static bool tco_context_touch_event(tco_context_t ctx,
                                    screen_event_t window) {
    if(!ctx)
        return false;
    //TODO:
    return false;
}

static tco_control_t tco_context_control_at(tco_context_t ctx,
                                            int x,
                                            int y) {
    if(!ctx)
        return NULL;
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
                     const char* filename) {
    tco_context_t c = (tco_context_t)context;
    return tco_context_load_controls(c, filename);
}

int tco_loadcontrols_default(tco_context_t context) {
    tco_context_t c = (tco_context_t)context;
    return tco_context_load_default_controls(c);
}

int tco_swipedown(tco_context_t context,
                  screen_window_t window) {
    tco_context_t c = (tco_context_t)context;
    return tco_context_show_configuration(c, window);
}

int tco_showlabels(tco_context_t context,
                   screen_window_t window) {
    tco_context_t c = (tco_context_t)context;
    return tco_context_show_labels(c, window);
}

int tco_touch(tco_context_t context,
              screen_event_t event) {
    tco_context_t c = (tco_context_t)context;
    bool handled = tco_context_touch_event(c, event);
    if (handled)
    {
        return TCO_SUCCESS;
    }
    return TCO_UNHANDLED;
}

void tco_shutdown(tco_context_t context) {
    tco_context_t c = (tco_context_t)context;
    tco_context_free(c);
}
