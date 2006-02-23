/* 
 * Copyright (C) 2005 Sasha Vasko <sasha at aftercode.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#define LOCAL_DEBUG
#include "../configure.h"

#include "../include/afterbase.h"
#include "../libAfterImage/afterimage.h"
#include "../libAfterStep/asapp.h"
#include "../libAfterStep/screen.h"

#include <unistd.h>		   

#include "asgtk.h"
#include "asgtkai.h"
#include "asgtkxmloptlist.h"

/*  local function prototypes  */
static void asgtk_xml_opt_list_class_init (ASGtkXmlOptListClass *klass);
static void asgtk_xml_opt_list_init (ASGtkXmlOptListDir *iv);
static void asgtk_xml_opt_list_dispose (GObject *object);
static void asgtk_xml_opt_list_finalize (GObject *object);
static void asgtk_xml_opt_list_style_set (GtkWidget *widget, GtkStyle  *prev_style);


/*  private variables  */
static GtkScrolledWindowClass *parent_class = NULL;

GType
asgtk_xml_opt_list_get_type (void)
{
  	static GType id_type = 0;

  	if (! id_type)
    {
    	static const GTypeInfo id_info =
      	{
        	sizeof (ASGtkXmlOptListClass),
        	(GBaseInitFunc)     NULL,
        	(GBaseFinalizeFunc) NULL,
			(GClassInitFunc)    asgtk_xml_opt_list_class_init,
        	NULL,           /* class_finalize */
        	NULL,           /* class_data     */
        	sizeof (ASGtkXmlOptList),
        	0,              /* n_preallocs    */
        	(GInstanceInitFunc) asgtk_xml_opt_list_init,
      	};

      	id_type = g_type_register_static (	GTK_TYPE_SCROLLED_WINDOW,
        	                                "ASGtkXmlOptList",
            	                            &id_info, 0);
    }

  	return id_type;
}

static void
asgtk_xml_opt_list_class_init (ASGtkXmlOptListClass *klass)
{
  	GObjectClass   *object_class = G_OBJECT_CLASS (klass);
  	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  	parent_class = g_type_class_peek_parent (klass);

  	object_class->dispose   = asgtk_xml_opt_list_dispose;
  	object_class->finalize  = asgtk_xml_opt_list_finalize;

  	widget_class->style_set = asgtk_xml_opt_list_style_set;

}

static void
asgtk_xml_opt_list_init (ASGtkXmlOptList *self)
{
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self),
				    				GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	self->flags = ASGTK_XmlOptList_DefaultFlags ; 
	self->fulldirname = NULL ; 
	self->entries = NULL ;

	self->configfilename = NULL ;
	self->opt_list_context = NULL ;

}

static void
asgtk_xml_opt_list_dispose (GObject *object)
{
  	ASGtkXmlOptList *self = ASGTK_IMAGE_DIR (object);
	if( self->configfilename ) 
		free( self->configfilename );
	destroy_asimage_list( &(self->entries) );
  	G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
asgtk_xml_opt_list_finalize (GObject *object)
{
  	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
asgtk_xml_opt_list_style_set (GtkWidget *widget,
                              GtkStyle  *prev_style)
{
  /* ASGtkImageDir *id = ASGTK_IMAGE_DIR (widget); */

  GTK_WIDGET_CLASS (parent_class)->style_set (widget, prev_style);
}

static void
asgtk_xml_opt_list_sel_handler(GtkTreeSelection *selection, gpointer user_data)
{
  	ASGtkXmlOptList *self = ASGTK_IMAGE_DIR(user_data); 
	GtkTreeIter iter;
	GtkTreeModel *model;

  	if (gtk_tree_selection_get_selected (selection, &model, &iter)) 
	{
		gpointer p = NULL ;
    	gtk_tree_model_get (model, &iter, ASGTK_XmlOptList_Cols, &p, -1);
		self->curr_selection = (ASImageListEntry*)p;
  	}else
		self->curr_selection = NULL ;
		
	if( self->sel_change_handler )
		self->sel_change_handler( self, self->sel_change_user_data ); 
}

/*  public functions  */
GtkWidget *
asgtk_xml_opt_list_new ()
{
	ASGtkXmlOptList *self;
	GtkTreeSelection *selection;
	const char *column_names[ASGTK_XmlOptList_Cols] = {"Module", "Keyword", "Id", "Value"};
	int i ;
	int default_columns = ASGTK_XmlOptList_DefaultFlags&ASGTK_XmlOptList_Cols_All ; 
  	
    self = g_object_new (ASGTK_TYPE_XML_OPT_LIST, NULL);

	self->tree_view = GTK_TREE_VIEW(gtk_tree_view_new());
	self->tree_model = GTK_TREE_MODEL(gtk_list_store_new (ASGTK_XmlOptList_Cols+1, 	G_TYPE_STRING, 
															    				   	G_TYPE_STRING, 
																  			   		G_TYPE_STRING, 
																			   		G_TYPE_STRING, 
																			   		G_TYPE_POINTER));

	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (self), GTK_SHADOW_IN);      
	
	gtk_container_add (GTK_CONTAINER(self), GTK_WIDGET(self->tree_view));
    gtk_tree_view_set_model (self->tree_view, self->tree_model);
    gtk_widget_show (GTK_WIDGET(self->tree_view));
	for( i = 0 ; i < ASGTK_XmlOptList_Cols ; ++i ) 
	{
		GtkCellRenderer *renderer = gtk_cell_renderer_text_new ();
	    self->columns[i] = gtk_tree_view_column_new_with_attributes (column_names[i], renderer, "text", i, NULL);
    }
	clear_flags( self->flags, ASGTK_XmlOptList_Cols_All );
	asgtk_xml_opt_list_set_columns( self, default_columns );
	
	selection = gtk_tree_view_get_selection(self->tree_view);
	gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);
   	g_signal_connect (selection, "changed",  G_CALLBACK (asgtk_xml_opt_list_sel_handler), self);
	
	colorize_gtk_tree_view_window( GTK_WIDGET(self) );

	LOCAL_DEBUG_OUT( "created image ASGtkXmlOptList object %p", self );	
	return GTK_WIDGET (id);
}

void  
asgtk_xml_opt_list_set_columns( ASGtkXmlOptList *self, ASFlagType columns )
{
	int i; 	
	for( i = 0 ; i < ASGTK_XmlOptList_Cols ; ++i ) 
	{	
		ASFlagType flag = 0x01<<i ;
		if( get_flags(columns, flag) )
		{	
			gtk_tree_view_insert_column (self->tree_view, GTK_TREE_VIEW_COLUMN (self->columns[i]), i);
			set_flags( self->flags, flag );	
		}else if( get_flags( self->flags, flag ) ) 
		{
			gtk_tree_view_remove_column (self->tree_view, GTK_TREE_VIEW_COLUMN (self->columns[i]));
			clear_flags( self->flags, flag );	
		}	 
	}
}

void  
asgtk_xml_opt_list_set_list_all( ASGtkXmlOptList *self, Bool enable )
{
	if( enable && get_flags(self->flags, ASGTK_XmlOptList_ListForeign ) )
		return ;
	if( !enable && !get_flags(self->flags, ASGTK_XmlOptList_ListForeign ) )
		return ;
	if( enable ) 
		set_flags(self->flags, ASGTK_XmlOptList_ListForeign );
	else
		clear_flags(self->flags, ASGTK_XmlOptList_ListForeign );

	asgtk_xml_opt_list_refresh( self );		 		
}


void  
asgtk_xml_opt_list_set_path( ASXmlOptList *self, char *fulldirname )
{
	g_return_if_fail (ASGTK_IS_XML_OPT_LIST (self));
	
	if( self->fulldirname == NULL && fulldirname == NULL ) 
		return;
	if( self->fulldirname && fulldirname && strcmp(self->fulldirname, fulldirname)== 0  ) 
		return;
	if( self->fulldirname  ) 
	{	
		free( self->fulldirname );
		self->fulldirname = NULL ; 
	}

	if( fulldirname ) 
		self->fulldirname = mystrdup(fulldirname);
	
	asgtk_xml_opt_list_refresh( self );		 
}	 

void  
asgtk_xml_opt_list_set_title( ASGtkXmlOptList *self, const gchar *title )
{
	g_return_if_fail (ASGTK_IS_XML_OPT_LIST (self));
	gtk_tree_view_column_set_title( self->columns[0], title );	
}

void  
asgtk_xml_opt_list_set_sel_handler( ASGtkXmlOptList *self, _ASGtkXmlOptList_sel_handler sel_change_handler, gpointer user_data )
{
	g_return_if_fail (ASGTK_IS_XML_OPT_LIST (self));

	self->sel_change_handler = sel_change_handler ; 
	self->sel_change_user_data = user_data ;
	
}

struct ASImageListEntry *
asgtk_xml_opt_list_get_selection(ASGtkXmlOptList *self )
{
	ASImageListEntry *result = NULL ; 

	if( ASGTK_IS_XML_OPT_LIST (self) )
		result = ref_asimage_list_entry(self->curr_selection);
	
	return result;	 
}


