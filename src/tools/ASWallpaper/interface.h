#ifndef ASWALLPAPER_INTERFACE_H_HEADER_INCLUDED
#define ASWALLPAPER_INTERFACE_H_HEADER_INCLUDED
/*
 * DO NOT EDIT THIS FILE - it is generated by Glade.
 */

typedef struct ASWallpaperState
{
#define DISPLAY_SYSTEM_BACKS	(0x01<<0)  /* otherwise display private */	
	
	ASFlagType flags ; 
	
	GtkWidget   *main_window ; 

	GtkWidget   *list_hbox;	
	GtkWidget   *backs_list ;
	
	GtkWidget    *list_browse_button ;
	GtkWidget    *list_update_as_button ;

	GtkWidget    *list_preview ; 
	GtkWidget    *sel_del_button ;
	GtkWidget    *sel_apply_button ;
	GtkWidget    *make_xml_button ;
	GtkWidget    *edit_xml_button ;
	GtkWidget    *make_mini_button ;
	GtkWidget    *update_mini_button ;

	

	GtkWidget    *filechooser ;
	GtkWidget    *xml_editor ;

	GtkWidget    *new_solid_button ; 
	GtkWidget    *new_gradient_button ; 

#define INITIAL_PREVIEW_HEIGHT	300

	int preview_width, preview_height ;

	ASImageListEntry *private_backs_list ;
	int 			  private_backs_count;
	
}ASWallpaperState;

extern ASWallpaperState WallpaperState ;

void init_ASWallpaper();

GtkWidget* create_filechooserdialog2 (void);

#endif
