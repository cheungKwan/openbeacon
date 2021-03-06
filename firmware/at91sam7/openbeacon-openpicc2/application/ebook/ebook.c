#include <FreeRTOS.h>
#include <AT91SAM7.h>
#include <board.h>
#include <beacontypes.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>

#include <task.h>

#include "sdram.h"
#include "led.h"
#include "power.h"
#include "accelerometer.h"
#include "eink/eink.h"
#include "eink/eink_flash.h"
#include "ebook/event.h"
#include "touch/slider.h"

#include "image/splash.h"
#include "image/image.h"

extern const struct splash_image splash_splash_image;
extern const struct splash_image arrows_splash_image;
extern const struct splash_image directory_splash_image;

static signed char task_list[10*40]; /* 40 bytes per task, approx. 10 tasks */

#define DISPLAY_SHORT (EINK_CURRENT_DISPLAY_CONFIGURATION->vsize)
#define DISPLAY_LONG (EINK_CURRENT_DISPLAY_CONFIGURATION->hsize)

/* Warning: The actual display size must not exceed this definition. E.g.
 * assert( IMAGE_SIZE >= EINK_CURRENT_DISPLAY_CONFIGURATION->hsize * EINK_CURRENT_DISPLAY_CONFIGURATION->vsize );
 */
#define IMAGE_SIZE (1216*832)

#define CLICK_MAX_DURATION (250/portTICK_RATE_MS)
#define SCROLL_SLIDER_LENGTH 10
#define SCROLL_SLIDER_DEAD_MIDDLE 4.5
#define SCROLL_SLIDER_DEAD_SPAN 1
#define SCROLL_HISTORY 3

#define SWIPE_MIN_DISTANCE_X (0.75*slider_x.part_diff)
#define SWIPE_MIN_DISTANCE_Y (0.25*slider_y.part_diff)

#define DIRECTORY_ENTRIES 10

#define ROUNDUP4(a) (4*((a+3)/4))

static struct image image_buffer[10];
static uint8_t  __attribute__ ((section (".sdram"))) _image_data[10][IMAGE_SIZE];
static unsigned char eink_mgmt_data[10240] __attribute__ ((section (".sdram")));

static int is_portrait_oriented(enum ORIENTATION orientation)
{
	switch(orientation) {
	case ORIENTATION_Y_DOWN: /* Landscape */
	case ORIENTATION_Y_UP:
		return 0;
		break;
	case ORIENTATION_X_DOWN: /* Portrait */
	case ORIENTATION_X_UP:
	default:
		return 1;
		break;
	}

}

static void get_display_sizes(enum ORIENTATION orientation, unsigned int *display_width, unsigned int *display_height)
{
	if(is_portrait_oriented(orientation)) {
		*display_width = DISPLAY_SHORT;
		*display_height = DISPLAY_LONG;
	} else {
		*display_width = DISPLAY_LONG;
		*display_height = DISPLAY_SHORT;
	}
}

static int can_display(enum ORIENTATION orientation, const image_t image)
{
	unsigned int display_width, display_height;
	get_display_sizes(orientation, &display_width, &display_height);
	if(image->width > (int)display_width || image->height > (int)display_height)
		return 0;
	return 1;
}

#if 0
#define FLAG_UPDATE_FULL 0
#define FLAG_UPDATE_PART 1
#define FLAG_WAVEFORM_GC 0
#define FLAG_WAVEFORM_GU 2
static void display_unpacked_image(enum ORIENTATION orientation, const struct image_buffer *image,
		int flags)
{
	unsigned int display_width, display_height;
	get_display_sizes(orientation, &display_width, &display_height);

	unsigned int offset_x = (display_width - image->width)/2;
	unsigned int offset_y = (display_height - image->height)/2;

	if(offset_x >= display_width || offset_y >= display_height)
		return; /* Can't display in this mode */

	eink_display_streamed_area_begin(EINK_PACKED_MODE_1BYTE, offset_x, offset_y, image->width, image->height);
	led_set_red(1);
	eink_display_streamed_image_update(image->data, image->width * image->height);
	led_set_red(0);
	if(eink_display_streamed_image_end() > 0) {
		printf("Image load ok\n");
		int waveform = (flags & FLAG_WAVEFORM_GU) ? EINK_WAVEFORM_GU : EINK_WAVEFORM_GC;
		if(flags & FLAG_UPDATE_PART) {
			eink_display_update_part(waveform);
		} else {
			eink_display_update_full(waveform);
		}
	} else {
		printf("Image load failed\n");
	}
}
#endif

#ifdef TXTR_PLEXIGLASS
const int rotation_map[] = {
		[ORIENTATION_Y_UP] = ROTATION_MODE_180,
		[ORIENTATION_X_UP] = ROTATION_MODE_90,
		[ORIENTATION_Y_DOWN] = ROTATION_MODE_0,
		[ORIENTATION_X_DOWN] = ROTATION_MODE_270,
};
#else
const int rotation_map[] = {
		[ORIENTATION_Y_UP] = ROTATION_MODE_0,
		[ORIENTATION_X_UP] = ROTATION_MODE_90,
		[ORIENTATION_Y_DOWN] = ROTATION_MODE_180,
		[ORIENTATION_X_DOWN] = ROTATION_MODE_270,
};
#endif

eink_image_buffer_t blank_buffer[4],
	splash_buffer[4],
	directory_buffer[4],
	arrows_buffer[4];

struct {
	eink_image_buffer_t *buffer;
	unsigned int count;
	char *description;
} _buffers[] = {
		{blank_buffer, 4, "blank"},
		{splash_buffer, 4, "splash"},
		{directory_buffer, 4, "directory"},
		{arrows_buffer, 4, "arrows"},
};

static void clear_screen(void)
{
	eink_job_t job;
	eink_job_begin(&job, 0);
	eink_job_add(job, blank_buffer[0], WAVEFORM_MODE_GC, UPDATE_MODE_FULL);
	eink_job_commit(job);
}

static void update_scroll_position(enum ORIENTATION orientation, int *old_position, int new_position)
{
	int direction;
	int pos_screen, opos_screen;

	switch(rotation_map[orientation]) {
	case ROTATION_MODE_90:
		direction = 1;
		break;
	case ROTATION_MODE_270:
		direction = 0;
		break;
	default: /* No arrows in this rotation */
		return;
	}


	if(old_position != NULL && *old_position != -1) {
		if(direction == 0) opos_screen = *old_position;
		else opos_screen = DIRECTORY_ENTRIES - 1 - *old_position;

		int offset_y = 67.625*(float)opos_screen + 80;
		int offset_x = 10;
		int width = 20;
		int height = 50;

		eink_job_t job;
		eink_job_begin(&job, (direction<<8) | (*old_position) );
		eink_job_add_area(job, blank_buffer[orientation], WAVEFORM_MODE_DU, UPDATE_MODE_PART,
				offset_x, offset_y, width, height);
		eink_job_commit(job);
	}
	/* Display new arrow */
	if(direction == 0) pos_screen = new_position;
	else pos_screen = DIRECTORY_ENTRIES - 1 - new_position;

	int offset_y = 67.625*(float)pos_screen + 80;
	int offset_x = 10;
	int width = 20;
	int height = 50;

	eink_job_t job;
	eink_job_begin(&job, (direction<<8) | (new_position));
	eink_job_add_area(job, arrows_buffer[orientation], WAVEFORM_MODE_DU, UPDATE_MODE_PART,
			offset_x, offset_y, width, height);
	eink_job_commit(job);

	*old_position = new_position;
}

static void ebook_die_error(enum eink_error error)
{
	switch(error) {
	case EINK_ERROR_NOT_DETECTED:
		printf("eInk controller not detected\n"); break;
	case EINK_ERROR_NOT_SUPPORTED:
		printf("eInk controller not supported (unknown revision)\n"); break;
	case EINK_ERROR_COMMUNICATIONS_FAILURE:
		printf("eInk controller communications failure, check FPC cable and connector\n"); break;
	case EINK_ERROR_CHECKSUM_ERROR:
		printf("eInk controller waveform flash checksum failure\n"); break;
	default:
		printf("eInk controller initialization: unknown error\n"); break;
	}
	led_halt_blinking(2);
}

static int event_loop_running = 0;
static void ebook_task(void *params)
{
	(void)params;
	int error, idle_time=0;
	int scroll_position_displayed = -1;
	enum {
		MODE_SPLASH,
		MODE_DIRECTORY,
	} menu_mode = MODE_SPLASH;
	vTaskDelay(100/portTICK_RATE_MS);
	enum ORIENTATION last_orientation_displayed = accelerometer_get_orientation();

	eink_mgmt_init(eink_mgmt_data, sizeof(eink_mgmt_data));

#if 0 /* FIXME Seems to be broken, sometimes (hangs at boot) */
	error = eink_flash_conditional_reflash();
	if(error > 0) { /* Not an error, but the controller was reinitialized and we must wait */
		vTaskDelay(5/portTICK_RATE_MS);
	} else if(error < 0) {
		ebook_die_error(-error);
	}
#endif

	vTaskDelay(5000);
	printf("A\n");
	eink_controller_reset();
	printf("B\n");

	error=eink_controller_init();
	if(error < 0) ebook_die_error(-error);
	printf("C\n");
	
	{
		unsigned int i;
		for(i=0; i<(sizeof(image_buffer)/sizeof(image_buffer[0])); i++) {
			image_buffer[i].size = sizeof(_image_data[i]);
			image_buffer[i].data = _image_data[i];
		}
	}
	{
		unsigned int i, j;
		for(i=0; i < (sizeof(_buffers)/sizeof(_buffers[0])); i++) {
			for(j=0; j < _buffers[i].count; j++) {
				if((error=eink_image_buffer_acquire(&(_buffers[i].buffer[j]))) < 0) {
					printf("Reason: %i\nCould not acquire image buffer: %s %i\n", error, _buffers[i].description, j);
					led_halt_blinking(3);
				}
			}
		}
	}

	/*
	 * splash_image, directory_image, arrows_image are in the flash
	 * image_buffer[] is in the microcontroller SDRAM
	 * splash_buffer, directory_buffer, arrows_buffer, blank_buffer are in the display controller SDRAM
	 */
	
	/* Empty image in image buffer 0 */
	image_buffer[0].bits_per_pixel = IMAGE_BPP_8;
	image_create_solid(&image_buffer[0], 0xff, DISPLAY_LONG, DISPLAY_SHORT);
	
	/* Splash image in image buffer 1 */
	image_unpack_splash(&image_buffer[1], &splash_splash_image);
	
	/* Directory image in buffer 2 */
	image_unpack_splash(&image_buffer[2], &directory_splash_image);
	
	/* Prepared arrows in buffer 3 */
	image_unpack_splash(&image_buffer[3], &arrows_splash_image);
	
	portTickType start = xTaskGetTickCount(), stop;
	{
		int orient;
		for(orient = 0; orient < 4; orient ++) {
			unsigned int width, height;
			get_display_sizes(orient, &width, &height);

			/* Clear the splash_buffer by loading from the white image into it, then paint the logo
			 * by loading from the splash_image
			 */
			eink_image_buffer_load(splash_buffer[orient], 
					image_get_bpp_as_pack_mode(&image_buffer[0]), rotation_map[orient],
					image_buffer[0].data, image_buffer[0].rowstride*image_buffer[0].height);
			
			eink_image_buffer_load_area(splash_buffer[orient], 
					image_get_bpp_as_pack_mode(&image_buffer[1]), rotation_map[orient],
					(width-image_buffer[1].width)/2, (height-image_buffer[1].height)/2,
					image_buffer[1].width, image_buffer[1].height,
					image_buffer[1].data, image_buffer[1].rowstride*image_buffer[1].height);
			
			error = eink_image_buffer_load(blank_buffer[orient], 
					image_get_bpp_as_pack_mode(&image_buffer[0]), rotation_map[orient],
					image_buffer[0].data, image_buffer[0].rowstride*image_buffer[0].height);
			
			eink_image_buffer_load(directory_buffer[orient], 
					image_get_bpp_as_pack_mode(&image_buffer[2]), rotation_map[orient],
					image_buffer[2].data, image_buffer[2].rowstride*image_buffer[2].height);
			
			eink_image_buffer_load(arrows_buffer[orient], 
					image_get_bpp_as_pack_mode(&image_buffer[3]), rotation_map[orient],
					image_buffer[3].data, image_buffer[3].rowstride*image_buffer[3].height);
		}
	}
	stop = xTaskGetTickCount();
	printf("Images loaded in %li ticks\n", (long)(stop-start));

	if(error >= 0) {
		printf("White image load ok\n");
		eink_job_t job;
		eink_job_begin(&job, 0);
		eink_job_add(job, blank_buffer[0], WAVEFORM_MODE_INIT, UPDATE_MODE_FULL);
		eink_job_commit(job);
	} else {
		printf("White image load error %i: %s\n", error, strerror(-error));
	}

	int i=0;
	eink_job_t job;
	eink_image_buffer_t currently_displayed = splash_buffer[last_orientation_displayed];

	eink_job_begin(&job, 0);
	eink_job_add(job, currently_displayed, WAVEFORM_MODE_GC, UPDATE_MODE_FULL);
	eink_job_commit(job);

	event_t received_event;
	event_loop_running = 1;
	while(1) {
		received_event.class = EVENT_NONE;
		event_receive(&received_event, 1000/portTICK_RATE_MS);
		i++;

		int update_display = 0, part = 0;

		switch(received_event.class) {
		case EVENT_ROTATED:
			if(received_event.param != last_orientation_displayed) {
				if(menu_mode == MODE_SPLASH) {
					last_orientation_displayed = received_event.param;
					currently_displayed = splash_buffer[last_orientation_displayed];
					update_display = 1;
					part = 0;
				} else if(menu_mode == MODE_DIRECTORY) {
					if(can_display(received_event.param, &image_buffer[2])) {
						last_orientation_displayed = received_event.param;
						currently_displayed = directory_buffer[last_orientation_displayed/2];
						update_display = 1;
					}
				}
			}
			break;
		case EVENT_CLICKED:
			if(menu_mode == MODE_SPLASH && received_event.param != CLICK_MAGIC) {
				menu_mode = MODE_DIRECTORY;
				if(!is_portrait_oriented(last_orientation_displayed))
					last_orientation_displayed = ORIENTATION_X_UP;
				currently_displayed = directory_buffer[last_orientation_displayed/2];
				update_display = 1;
			}
			if(menu_mode != MODE_SPLASH && received_event.param == CLICK_MAGIC) {
				menu_mode = MODE_SPLASH;
				currently_displayed = splash_buffer[last_orientation_displayed];
				last_orientation_displayed = accelerometer_get_orientation();
				update_display = 1;
				scroll_position_displayed = -1;
			}
			printf("Have click %i\n", received_event.param);
			break;
		case EVENT_SCROLL_RELATIVE_Y:
			if(menu_mode == MODE_DIRECTORY) {
				int new_position = scroll_position_displayed + received_event.param;
				if(new_position >= 0 && new_position < DIRECTORY_ENTRIES)
					update_scroll_position(last_orientation_displayed, &scroll_position_displayed, new_position);
			}
			printf("Have scroll: %i\n", received_event.param);
			break;
		case EVENT_POWEROFF:
			clear_screen();
			while(eink_job_count_pending() > 0) vTaskDelay(10/portTICK_RATE_MS);
			power_off();
		default:
			break;
		}

		if(received_event.class == EVENT_NONE && !power_is_usb_connected()) {
			idle_time++;
		} else {
			idle_time=0;
		}
		if(idle_time > 600) {
			clear_screen();
			while(eink_job_count_pending() > 0) vTaskDelay(10/portTICK_RATE_MS);
			power_off();
		}

		if(update_display) {
			eink_job_begin(&job, 0);
			eink_job_add(job, currently_displayed, WAVEFORM_MODE_GC, part ? UPDATE_MODE_PART : UPDATE_MODE_FULL);
			eink_job_commit(job);
		}

		if(i==10) {
			vTaskList(task_list);
			printf("%s\n", task_list);
		}
	}
}

static void orientation_changed_cb(enum ORIENTATION new_orientation)
{
	event_send(EVENT_ROTATED, new_orientation);
}

static void power_pressed_cb(void)
{
	if(event_loop_running)
		event_send(EVENT_POWEROFF, 0);
	else
		power_off();
}

/* Transform the slider updates into useful events.
 * Distinguish:
 *  + Click (touchdown and release within 150ms)
 *  + Relative scroll Y
 *  + Swipe scroll X */
static struct {
	int current_scroll_position;
	long int last_touchdown_time;
	int button1, button2, xtouching, ytouching, magic;
	int currently_touching;
	int slider_history[SCROLL_HISTORY]; int slider_history_index;
	int scroll_sent;
	int y_touchdown_position, x_touchdown_position, x_touch_position;
} slider_info = {
		.current_scroll_position = -1,
		.y_touchdown_position = -1,
		.x_touchdown_position = -1,
		.slider_history = {-1},
};
#define MAX(a,b) ((a)<(b)?(b):(a))
static void slider_update_cb(struct slider_state *state)
{
	char touching = (state->buttons.button1 || state->buttons.button2
			|| state->buttons.ytouching || state->buttons.xtouching);
	int i;
	if(!slider_info.currently_touching && touching) {
		slider_info.last_touchdown_time = xTaskGetTickCount();
		slider_info.currently_touching = 1;
	}

	if(slider_info.currently_touching) {
		if(state->buttons.xtouching) { /* Touching slider, scroll/swipe X */
			slider_info.xtouching++;

			if(slider_info.x_touchdown_position == -1) {
				slider_info.x_touchdown_position = state->xval;
			}
			slider_info.x_touch_position = state->xval;
		}

		if(state->buttons.ytouching) { /* Touching slider, scroll Y */
			slider_info.ytouching++;
			/* Classify touch into one of DIRECTORY_ENTRIES bins */
			int position = (state->yval + (slider_y.val_max/(SCROLL_SLIDER_LENGTH-1)/2)) / (slider_y.val_max/(SCROLL_SLIDER_LENGTH-1));

			/* Keep a history of last SCROLL_HISTORY positions to low-pass filter the signal */
			slider_info.slider_history[slider_info.slider_history_index] = position;
			slider_info.slider_history_index = (slider_info.slider_history_index+1) % SCROLL_HISTORY;

			for(i=0; i<SCROLL_HISTORY; i++) {
				if(slider_info.slider_history[i] != position) {
					position = slider_info.current_scroll_position;
					break;
				}
			}

			if( position != slider_info.current_scroll_position && position != -1 ) {
				/* only act when the low-pass filter says so */
				if( slider_info.y_touchdown_position == -1)
					slider_info.y_touchdown_position = position;

				/* Don't report scrolls that seem to start in
				 * SCROLL_SLIDER_DEAD_MIDDLE +/- SCROLL_SLIDER_DEAD_SPAN, unless they move out of
				 * that area. This is to prevent swipes from reporting bogus scrolls */
				int dead_area = !slider_info.scroll_sent
				&& (slider_info.y_touchdown_position >= SCROLL_SLIDER_DEAD_MIDDLE-SCROLL_SLIDER_DEAD_SPAN)
				&& (slider_info.y_touchdown_position <= SCROLL_SLIDER_DEAD_MIDDLE+SCROLL_SLIDER_DEAD_SPAN);
				if(dead_area && ( (position < SCROLL_SLIDER_DEAD_MIDDLE-SCROLL_SLIDER_DEAD_SPAN)
						|| (position > SCROLL_SLIDER_DEAD_MIDDLE+SCROLL_SLIDER_DEAD_SPAN) ) )
					dead_area = 0;
				if(dead_area) {
					slider_info.y_touchdown_position = -1;
				} else {
					if( ( slider_info.current_scroll_position != position && xTaskGetTickCount() - slider_info.last_touchdown_time > CLICK_MAX_DURATION )
							|| ( slider_info.current_scroll_position != -1 && abs( slider_info.current_scroll_position - position ) > 1 ) ) {
						if( slider_info.current_scroll_position != -1 ) {
							event_send(EVENT_SCROLL_RELATIVE_Y, position-slider_info.current_scroll_position);
							slider_info.scroll_sent = 1;
						}
						slider_info.current_scroll_position = position;
					}
				}
			}
		} else if(state->buttons.button1) slider_info.button1++;
		else if(state->buttons.button2) slider_info.button2++;

		if(state->buttons.button1 && state->buttons.button2) slider_info.magic++;
	}

	if(slider_info.currently_touching && !touching) {
		int have_swipe = (slider_info.xtouching > 0 && abs(slider_info.x_touchdown_position-slider_info.x_touch_position) >= SWIPE_MIN_DISTANCE_X);
		if((xTaskGetTickCount() - slider_info.last_touchdown_time < CLICK_MAX_DURATION) ||
				(slider_info.magic > 0) || have_swipe) {
			/* This is a click */
			enum click_type type = -1;

			/* Only send through at most one click per touchdown/touchup sequence. Use
			 * the button that has been reported as touched most often */
			int max_clicks = MAX(MAX(MAX(slider_info.ytouching, slider_info.magic),
					MAX(slider_info.button1, slider_info.button2)), slider_info.xtouching);

			if(slider_info.magic > 0) type = CLICK_MAGIC;
			else if(have_swipe) type = slider_info.x_touchdown_position > slider_info.x_touch_position ? SWIPE_SLIDER_X_1 : SWIPE_SLIDER_X_2;
			else if(slider_info.button1 == max_clicks) type = CLICK_BUTTON_1;
			else if(slider_info.button2 == max_clicks) type = CLICK_BUTTON_2;
			else if(slider_info.ytouching == max_clicks && !slider_info.scroll_sent) type = CLICK_SLIDER_Y;
			else goto dont_send;

			event_send(EVENT_CLICKED, type);
			dont_send: ;

		} else {
			//printf("Too long: %lu\n", xTaskGetTickCount() - slider_info.last_touchdown_time);
		}
		slider_info.currently_touching = 0;
		slider_info.button1 = slider_info.button2 = slider_info.magic
		= slider_info.ytouching = slider_info.xtouching = slider_info.scroll_sent = 0;
		slider_info.x_touchdown_position = slider_info.y_touchdown_position
		= slider_info.current_scroll_position = -1;
		for(i=0; i<SCROLL_HISTORY; i++)
			slider_info.slider_history[i] = -1;
	}
}

int ebook_init(void)
{
	int r;
	if( (r=event_init()) < 0)
		return r;

	if( (r=accelerometer_register_orientation_changed_callback(orientation_changed_cb)) < 0)
		return r;
	if( (r=power_set_pressed_callback(power_pressed_cb)) < 0)
		return r;
	if( (r=slider_register_slider_update_callback(slider_update_cb)) < 0)
		return r;

	xTaskCreate(ebook_task, (signed portCHAR *) "EBOOK TASK", TASK_EBOOK_STACK,
			NULL, TASK_EBOOK_PRIORITY, NULL);

	return 0;
}
