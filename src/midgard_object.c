/* 
Copyright (C) 2004, 2005, 2006, 2007, 2008, 2009, 2010 Piotr Pokora <piotrek.pokora@gmail.com>
Copyright (C) 2004 Alexander Bokovoy <ab@samba.org>

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU Lesser General Public License as published
by the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <config.h>
#include <stdlib.h>
#include "midgard_defs.h"
#include "midgard_type.h"
#include "midgard_reflector_object.h"
#include "midgard_metadata.h"
#include "query_builder.h"
#include "midgard_timestamp.h"
#include "midgard_datatypes.h"
#include "midgard_quota.h"
#include "schema.h"
#include "midgard_reflection_property.h"
#include "midgard_error.h"
#include "midgard_core_object.h"
#include "midgard_core_object_class.h"
#include "midgard_core_query.h"
#include "midgard_connection.h"
#include "midgard_collector.h"
#include "guid.h"
#include "midgard_dbobject.h"
#include "midgard_user.h"
#include "midgard_dbus.h"
#include "midgard_core_query.h"
#include "midgard_core_query_builder.h"
#include "midgard_core_metadata.h"
#include "midgard_object_parameter.h"
#include "midgard_object_attachment.h"
#include "midgard_schema_object_factory.h"
#include "midgard_schema_object_tree.h"
#include <sql-parser/gda-sql-parser.h>

GType _midgard_attachment_type = 0;
static gboolean signals_registered = FALSE;

static GObjectClass *__mgdschema_parent_class = NULL;
static GObject *__mgdschema_object_constructor (GType type,
		guint n_construct_properties,
		GObjectConstructParam *construct_properties);
static void __mgdschema_object_dispose (GObject *object);

static void __add_core_properties(MgdSchemaTypeAttr *type_attr);
static gint __insert_or_update_records(MidgardObject *object, const gchar *table, gint query_type, const gchar *where);

enum {
	MIDGARD_PROPERTY_NULL = 0,
	MIDGARD_PROPERTY_GUID,
	MIDGARD_PROPERTY_METADATA
};

enum {
	OBJECT_IN_TREE_NONE = 0,
	OBJECT_IN_TREE_DUPLICATE,
};

enum {
	_SQL_QUERY_CREATE = 0,
	_SQL_QUERY_UPDATE
};

#define __dbus_send(_obj, _action) {						\
	MidgardConnection *__mgd = MGD_OBJECT_CNC (_obj);			\
	if (MGD_CNC_DBUS(__mgd)) {						\
		gchar *_dbus_path = g_strconcat("/mgdschema/", 			\
				G_OBJECT_TYPE_NAME(G_OBJECT(_obj)), 		\
				"/", _action, NULL); 				\
		midgard_core_dbus_send_serialized_object(_obj, _dbus_path); 	\
		g_free(_dbus_path);						\
	}									\
}

static GParamSpec **_midgard_object_class_paramspec()
{
	/* Hardcode MidgardRepligardClass parameters. Those will be inherited with MidgardObject 
	 * and thus do not bother about Midgard basic and core parameters later.
	 * We also should hardcode these parameters to keep one real Midgard object
	 * which has "one logic" properties and methods.
	 */
  
	/* Last value is 'params[n]+1' */
	GParamSpec **params = g_malloc(sizeof(GParamSpec*)*4);
	params[0] = g_param_spec_string ("guid", "", "GUID identifier of the object", "", G_PARAM_READABLE);
	params[1] = g_param_spec_object ("metadata", "",  "midgard_metadata for the object", MIDGARD_TYPE_METADATA, G_PARAM_READWRITE);
	params[2] = g_param_spec_string ("action", "", "Last action done to the object", "", G_PARAM_READWRITE);    
	params[3] = NULL;
	  
	return params;     
}

/* Initialize instance for all types that are not MidgardRepligardClass type. */
static void 
__midgard_object_instance_init (GTypeInstance *instance, gpointer g_class)
{	
	MidgardObject *self = (MidgardObject *) instance;
	
	/* Midgard Object Private */
	self->priv = g_new0 (MidgardObjectPrivate, 1);
	self->priv->action = NULL;
	self->priv->exported = NULL;
	self->priv->imported = NULL;
	self->priv->parameters = NULL;
	self->priv->_params = NULL;
}
                
/* AB: This is shortcut macro for traversing through class hierarchy (from child to ancestor)
 * until we reach GObject. On each iteration we fetch private data storage for the object
 * according to the current class type and run supplied code.
 * This allows us to implement dynamic subclassing cast which is done usually via compiled 
 * time structure casts in appropriate class-specific hooks for each class in a hierarchy.
 * As our object classes have the same structure at each level of inheritance, we are able
 * to use the same triple of functions for get_property(), set_property(), and finalize()
 * for each class.
 */
#define G_MIDGARD_LOOP_HIERARCHY_START	\
	do {	\
		if (current_type == MIDGARD_TYPE_OBJECT) break; \
		priv = G_TYPE_INSTANCE_GET_PRIVATE (object, current_type, MgdSchemaTypeAttr);  \

#define G_MIDGARD_LOOP_HIERARCHY_STOP	\
		current_type = g_type_parent(current_type);	\
	} while (current_type != MIDGARD_TYPE_DBOBJECT);

/* AB: Handle property assignments. Despite its simplicity, this is *very* important function */
static void
__midgard_object_set_property (GObject *object, guint prop_id, 
    const GValue *value, GParamSpec   *pspec)
{	
	gint prop_id_local = 0;
	GType current_type = G_TYPE_FROM_INSTANCE(object);
	MgdSchemaTypeAttr *priv = G_TYPE_INSTANCE_GET_PRIVATE (object, current_type, MgdSchemaTypeAttr);
	MidgardObject *self = (MidgardObject *) object;
	MgdSchemaPropertyAttr *prop;
	MidgardConnection *mgd = MGD_OBJECT_CNC (MIDGARD_DBOBJECT (object));	

	if (midgard_core_object_property_refuse_private (mgd, priv, MIDGARD_DBOBJECT (object), pspec->name))
		return;

	switch (prop_id) {
			
		case MIDGARD_PROPERTY_GUID:
			g_debug("Setting guid property is not allowed");
			break;
				
		case MIDGARD_PROPERTY_METADATA:
			MGD_DBOBJECT_METADATA (self) = g_value_get_object (value);
			break;
			
		default:
			G_MIDGARD_LOOP_HIERARCHY_START
				prop_id_local = prop_id - priv->base_index - 1;
			if ((prop_id_local >= 0) && (prop_id_local < priv->num_properties)) {
				if (priv->num_properties) {
					prop = priv->properties[prop_id_local];
					if (!G_IS_VALUE(&prop->value)) {
						g_value_init(&prop->value, G_VALUE_TYPE(value));
						/* g_debug(" Set property %s", pspec->name );  */
					}
					g_value_copy(value, &prop->value);
				}
				return;
			}
			G_MIDGARD_LOOP_HIERARCHY_STOP
	}
}

/* Get object's property */
static void
__midgard_object_get_property (GObject *object, guint prop_id,
		GValue *value, GParamSpec   *pspec)
{
	gint prop_id_local = 0;
	GType current_type = G_TYPE_FROM_INSTANCE(object);
	MgdSchemaTypeAttr *priv = G_TYPE_INSTANCE_GET_PRIVATE (object, current_type, MgdSchemaTypeAttr);
	MidgardObject *self = (MidgardObject *) object;
	MidgardDBObjectClass *dbklass = MIDGARD_DBOBJECT_GET_CLASS (object);
	MidgardConnection *mgd = MGD_OBJECT_CNC (MIDGARD_DBOBJECT (object));
	GValue *pval;

	if (midgard_core_object_property_refuse_private (mgd, priv, MIDGARD_DBOBJECT (object), pspec->name))
		return;

	if (dbklass->dbpriv->get_property (MIDGARD_DBOBJECT (self), pspec->name, value))
		return;

	switch (prop_id) {

		case MIDGARD_PROPERTY_GUID:	
			g_value_set_string (value, MGD_OBJECT_GUID (self));
			break;
				
		case MIDGARD_PROPERTY_METADATA:	
			g_value_set_object (value, (MidgardMetadata *) MGD_DBOBJECT_METADATA (self));
			break;
		
		default:
			G_MIDGARD_LOOP_HIERARCHY_START
			prop_id_local = prop_id - priv->base_index - 1;
			if ((prop_id_local >= 0) && (
						prop_id_local < priv->num_properties)) {
				if (priv->num_properties) {
					if (priv->properties) {
						pval = &priv->properties[prop_id_local]->value;
						if (G_IS_VALUE (pval)) {	
							g_value_copy(pval, value);			
						} else {
							/* Initialize Midgardtimestamp.
							 * There's no need to do it in constructor. Just do it when it's really needed */
							if (G_VALUE_TYPE (value) == MGD_TYPE_TIMESTAMP) {				
								MidgardTimestamp *mt = midgard_timestamp_new ();
								g_value_init (pval, MGD_TYPE_TIMESTAMP);
								g_value_take_boxed (pval, mt);
								g_value_copy (pval, value);
							}
						}
					}
				}
				return;
			}
			G_MIDGARD_LOOP_HIERARCHY_STOP
	}
}

static void 
__mgdschema_object_dispose (GObject *object)
{
	MidgardObject *self = MIDGARD_OBJECT (object);

	if (MIDGARD_DBOBJECT (self)->dbpriv 
			&& (MGD_DBOBJECT_METADATA (self) != NULL 
				&& G_IS_OBJECT(MGD_DBOBJECT_METADATA (self)))) {
		/* Remove weak reference */
		g_object_remove_weak_pointer (G_OBJECT (self), (gpointer) MGD_DBOBJECT_METADATA (self));
	}

	/* Free object's parameters */
	if (self->priv->parameters != NULL) {
		GSList *_param = self->priv->parameters;
		for (; _param; _param = _param->next) {
			g_object_unref(_param->data);
		}
		g_slist_free(_param);
		self->priv->parameters = NULL;
	}

	__mgdschema_parent_class->dispose (object);
}

/* 
 * Finalizer for MidgardObject instance.
 * Cleans up all allocated data. As optimization, it handles all data
 * which belongs to its ancestors up to but not inluding GObject.
 * It is really makes no sense to call this function recursively
 * for each ancestor because we already know object's structure.
 * For GObject we call its finalizer directly.
 */
static void __mgdschema_object_finalize (GObject *object) 
{
	guint idx;
	MgdSchemaTypeAttr *priv;

	if (object == NULL)
		return;

	MidgardObject *self = (MidgardObject *)object;
	
	/* Free private struct members and MidgardTypePrivate */	
	g_free((gchar *)self->priv->action);
	g_free(self->priv->exported);
	g_free(self->priv->imported);
	
	if (self->priv->_params != NULL)
		g_hash_table_destroy(self->priv->_params);

	g_free(self->priv);
	self->priv = NULL;
	
	GType current_type = G_TYPE_FROM_INSTANCE(object); 

	G_MIDGARD_LOOP_HIERARCHY_START
		for (idx = 0; idx < priv->num_properties; idx++) {
			if (priv->properties[idx]) {
				if (G_IS_VALUE(&priv->properties[idx]->value)) {
					g_value_unset(&priv->properties[idx]->value);
				}
				g_free (priv->properties[idx]);
				priv->properties[idx] = NULL;
			}
		}
	if (priv->properties) {
		g_free (priv->properties);
		priv->properties = NULL;
	}
	G_MIDGARD_LOOP_HIERARCHY_STOP 
		
	/* Call parent's finalizer if it is there */
	{
		GObjectClass *parent_class = g_type_class_ref (current_type);		
		if (parent_class->finalize) {
			parent_class->finalize (object);
		}
		g_type_class_unref (parent_class);
	}
}

static gboolean _is_circular(MidgardObject *object)
{
	MidgardObjectClass *klass = MIDGARD_OBJECT_GET_CLASS(object);

	GParamSpec *pspec = 
		g_object_class_find_property(G_OBJECT_GET_CLASS(object), "id");

	/* There's no id property so we do not have up or parent reference */ 
	if (!pspec) 
		return FALSE;

	guint oid = 0;
	g_object_get(G_OBJECT(object), "id", &oid, NULL);

	/* Probably it's creation */ 
	if (oid == 0)
		return FALSE;
	
	const gchar *up_prop = midgard_reflector_object_get_property_parent (G_OBJECT_TYPE_NAME (object)); 

	/* there's goto statement, because we might want to check other 
	 * circular references in a near future */
	if (up_prop == NULL)
		goto _CHECK_IS_UP_CIRCULAR;

_CHECK_IS_UP_CIRCULAR:
	up_prop = midgard_reflector_object_get_property_up (G_OBJECT_TYPE_NAME (object)); 

	if (up_prop == NULL)
		return FALSE;

	/* Ignore, if up property is a link to different class */
	MidgardReflectionProperty *mrp = midgard_reflection_property_new (MIDGARD_DBOBJECT_CLASS (klass));
	const MidgardDBObjectClass *linked_klass = midgard_reflection_property_get_link_class (mrp, up_prop);
	g_object_unref (mrp);
	if (MIDGARD_DBOBJECT_CLASS (linked_klass) != MIDGARD_DBOBJECT_CLASS (klass))
		return FALSE;

	/* we checked up property so "parent" class is of the same type */
	GValue *idval = g_new0(GValue, 1);
	g_value_init(idval , G_TYPE_UINT);
	g_value_set_uint(idval, oid);
	MidgardCollector *mc = 
		midgard_collector_new(MGD_OBJECT_CNC (object), G_OBJECT_TYPE_NAME(object),
				up_prop, idval);
	
	midgard_collector_set_key_property(mc, "id", NULL);

	if (!midgard_collector_execute(mc)){
		g_object_unref(mc);
		return FALSE;
	}

	gchar **keys = 
		midgard_collector_list_keys(mc);

	g_object_unref(mc);

	if (keys == NULL)
		return FALSE;

	guint id_up = 0;
	g_object_get(G_OBJECT(object), up_prop, &id_up, NULL);

	guint i = 0;
	while(keys[i] != NULL) {
		
		if ((guint)atol(keys[i]) == id_up) {
		
			MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_TREE_IS_CIRCULAR);
			return TRUE;
		}

		i++;
	}
	
	return FALSE;
}

/* TODO, replace execute with distinct if needed */  
guint _object_in_tree(MidgardObject *object)
{
	GParamSpec *name_prop, *up_prop;
	GValue pval = {0,};
	const gchar *classname = G_OBJECT_TYPE_NAME (object);

	if (_is_circular(object))
		return OBJECT_IN_TREE_DUPLICATE;
	
	const gchar *upname = midgard_reflector_object_get_property_parent (classname);
	const gchar *unique_name = midgard_reflector_object_get_property_unique (classname);
	if (upname == NULL)
		upname = midgard_reflector_object_get_property_up (classname);

	if (upname == NULL)
		return OBJECT_IN_TREE_NONE;

	if (unique_name == NULL)
		return OBJECT_IN_TREE_NONE;
	
	up_prop = g_object_class_find_property(
			G_OBJECT_GET_CLASS(G_OBJECT(object)), upname);
	name_prop = g_object_class_find_property(
			G_OBJECT_GET_CLASS(G_OBJECT(object)), unique_name);

	if ((name_prop == NULL) || (up_prop == NULL))
		return OBJECT_IN_TREE_NONE;

	MidgardQueryBuilder *builder =
		midgard_query_builder_new(MGD_OBJECT_CNC (object),
				G_OBJECT_TYPE_NAME(object));
	/* Add up or parent constraint */
	g_value_init(&pval,up_prop->value_type);
	g_object_get_property(G_OBJECT(object), upname, &pval);
	midgard_query_builder_add_constraint(builder,
			upname,
			"=", &pval);
	g_value_unset(&pval);
			
	/* Add name property constraint */
	g_value_init(&pval,name_prop->value_type);
	g_object_get_property(G_OBJECT(object), unique_name, &pval);
	/* Check empty name, not null */
	const gchar *tmpstr = g_value_get_string(&pval);
	if (!tmpstr)
		tmpstr = "";
	if (g_str_equal(tmpstr, "")){
		g_object_unref(builder);
		g_value_unset(&pval);
		return OBJECT_IN_TREE_NONE;
	}
	midgard_query_builder_add_constraint(builder,
			unique_name,
			"=", &pval);
	g_value_unset(&pval);

	/* set limit so we do not have to check how many objects should be freed */
	midgard_query_builder_set_limit (builder, 1);	

	guint n_objects;
	GObject **ret_object = midgard_query_builder_execute(builder, &n_objects);

	g_object_unref (builder);

	/* Name is not duplicated, return */
	if (!ret_object)
		return FALSE;
	
	gboolean duplicate = TRUE;
	
	const gchar *retguid = MGD_OBJECT_GUID (ret_object[0]);
	const gchar *guid = MGD_OBJECT_GUID (object);

	if (g_str_equal(retguid, guid))
		duplicate = FALSE;

	g_object_unref(ret_object[0]);
	g_free(ret_object);
		
	if (duplicate) {

		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_DUPLICATE);
		return OBJECT_IN_TREE_DUPLICATE;
	}
	
	/* PP:I am not sure if we really need this for every object. 
	 * Static path for dynamic applications is needless.
	 * However path should be build or used for applications 
	 * which do not change paths for libraries snippets code
	 * */
	/* (void *) midgard_object_build_path(mobj); */
  
	return OBJECT_IN_TREE_NONE;
}

/**
 * midgard_object_set_guid:
 * @self: #MidgardObject instance
 * @guid: guid to set
 * 
 * Sets object's guid
 * 
 * Cases to return %FALSE:
 * <itemizedlist>
 * <listitem><para>
 * given guid already exists in database ( MGD_ERR_DUPLICATE ) 
 * </para></listitem>
 * <listitem><para>
 * given guid is invalid ( MGD_ERR_INVALID_PROPERTY_VALUE )
 * </para></listitem>
 * <listitem><para>
 * object has already set guid property ( MGD_ERR_INVALID_PROPERTY_VALUE ) 
 * </para></listitem>
 * <listitem><para>
 * object is deleted or purged ( MGD_ERR_OBJECT_DELETED or MGD_ERR_OBJECT_PURGED ) 
 * </para></listitem>
 * </itemizedlist>
 * 
 * Returns: %TRUE on success, %FALSE otherwise
 */
gboolean midgard_object_set_guid(MidgardObject *self, const gchar *guid)
{
	g_assert(self != NULL);
	g_assert(guid != NULL);
	
	MidgardConnection *mgd = MGD_OBJECT_CNC (self);
	MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (self), MGD_ERR_OK);
	
	if (MGD_OBJECT_GUID (self) != NULL) {
		
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (self), MGD_ERR_INVALID_PROPERTY_VALUE);
		return FALSE;
	}
	
	if (!midgard_is_guid(guid)) {
		
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (self), MGD_ERR_INVALID_PROPERTY_VALUE);
		return FALSE;
	}

	MidgardObject *dbobject = midgard_schema_object_factory_get_object_by_guid(MGD_OBJECT_CNC (self), guid);
	
	if (dbobject) {
		
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (self), MGD_ERR_DUPLICATE);
		g_object_unref(dbobject);
		return FALSE;
	}

	gint errcode = midgard_connection_get_error (mgd);
	if (!dbobject && (errcode == MGD_ERR_OBJECT_DELETED || errcode == MGD_ERR_OBJECT_PURGED)) 
	       return FALSE;	

	if (MGD_OBJECT_GUID (self))
	       g_free ((gchar *) MGD_OBJECT_GUID (self));
	MGD_OBJECT_GUID (self) = g_strdup(guid);

	return TRUE;
}

gboolean _midgard_object_update(MidgardObject *gobj, 
		_ObjectActionUpdate replicate)
{
	g_assert(gobj != NULL);
	
	gchar *fquery = "";
	const gchar *table;
	GString *sql = NULL;
	guint object_init_size = 0;
	
	if (MGD_OBJECT_GUID (gobj) == NULL)
		g_critical("Object's guid is NULL. Can not update");

	/* Get object's size as it's needed for size' diff.
	 * midgard_core_object_is_valid computes new size */
	MidgardMetadata *metadata = MGD_DBOBJECT_METADATA (gobj);
	if (metadata)
		object_init_size = metadata->priv->size;		

	if (!midgard_core_object_is_valid(gobj))
		return FALSE;

	MidgardConnection *mgd = MGD_OBJECT_CNC (gobj);

	MIDGARD_ERRNO_SET(mgd, MGD_ERR_OK);	

	/* Disable person check */
	/*
	if (gobj->person == NULL) {
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (gobj), MGD_ERR_ACCESS_DENIED);
		return FALSE;
	}
	*/
	
	if (MIDGARD_DBOBJECT (gobj)->dbpriv->storage_data == NULL) {
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (gobj), MGD_ERR_INTERNAL);
		return FALSE;
	}
	table = midgard_core_class_get_table(MIDGARD_DBOBJECT_GET_CLASS(gobj));
	if (table  == NULL) {
		/* Object has no storage defined. Return FALSE as there is nothing to update */
		g_warning("Object '%s' has no table defined!", 
				G_OBJECT_TYPE_NAME(gobj));
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (gobj), MGD_ERR_INTERNAL);
		return FALSE;
	}

	/* Check duplicates and parent field */
	if (_object_in_tree(gobj) == OBJECT_IN_TREE_DUPLICATE) 
		return FALSE;

	if (metadata) {
	
		guint object_size = metadata->priv->size;
	
		if (midgard_quota_size_is_reached(gobj, object_size)){
	
			MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (gobj), MGD_ERR_QUOTA);
			return FALSE;
		}
	}

	gint updated;
	gchar *where = midgard_core_query_where_guid(table, MGD_OBJECT_GUID (gobj));

	updated = __insert_or_update_records(gobj, table, GDA_SQL_STATEMENT_UPDATE, (const gchar *)where);

	g_free(where);
	
	if (updated == -1) {
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (gobj), MGD_ERR_INTERNAL);
		return FALSE;
	}

	midgard_quota_update(gobj, object_init_size);  

	/* Do not touch repligard table if replication is disabled */
	if (MGD_CNC_REPLICATION (mgd)) {
		sql = g_string_new("UPDATE repligard SET ");
		g_string_append_printf(sql,
				"typename='%s', object_action=%d WHERE guid='%s' ",	
				G_OBJECT_TYPE_NAME(gobj),
				MGD_OBJECT_ACTION_UPDATE,
				MGD_OBJECT_GUID (gobj));
		fquery = g_string_free(sql, FALSE);
		midgard_core_query_execute (MGD_OBJECT_CNC (gobj), fquery, FALSE);
		g_free(fquery);
	}

	/* Success, emit done signals */
	switch(replicate){
		
		case OBJECT_UPDATE_NONE:
			g_signal_emit(gobj, MIDGARD_OBJECT_GET_CLASS(gobj)->signal_action_updated, 0);
			break;
			
		case OBJECT_UPDATE_EXPORTED:
			g_signal_emit(gobj, MIDGARD_OBJECT_GET_CLASS(gobj)->signal_action_exported, 0);
			break;
			
		case OBJECT_UPDATE_IMPORTED:
			g_signal_emit(gobj, MIDGARD_OBJECT_GET_CLASS(gobj)->signal_action_imported, 0);
			break;
	}

	return TRUE;
}

/**
 * midgard_object_update:
 * @self: #MidgardObject instance
 *
 * Update object's record(s).
 * 
 * Internally such metadata properties are set (overwritten):
 * <itemizedlist>
 * <listitem><para>
 * revisor 
 * </para></listitem>
 * <listitem><para>
 * revision ( increased by one ) 
 * </para></listitem>
 * <listitem><para>
 * revised 
 * </para></listitem>
 * </itemizedlist>
 *  
 * Cases to return %FALSE:
 * <itemizedlist>
 * <listitem><para>
 * Property registered with #MGD_TYPE_GUID doesn't hold valid guid ( MGD_ERR_INVALID_PROPERTY_VALUE ) 
 * </para></listitem>
 * <listitem><para>
 * Object's class is registered with tree facilities and there is already such object in midgard tree ( MGD_ERR_DUPLICATE ) 
 * </para></listitem>
 * <listitem><para>
 * Quota is activated and its limit is reached ( MGD_ERR_QUOTA ) 
 * </para></listitem>
 * <listitem><para>
 * Unspecified internal error ( MGD_ERR_INTERNAL ) 
 * </para></listitem>
 * </itemizedlist>
 * 
 * Returns: %TRUE if object's record(s) is successfully updated, %FALSE otherwise.
 */ 
gboolean midgard_object_update(MidgardObject *self)
{
	g_signal_emit(self, MIDGARD_OBJECT_GET_CLASS(self)->signal_action_update, 0);
	gboolean rv =  _midgard_object_update(self, OBJECT_UPDATE_NONE);

	if (rv) {
		__dbus_send(self, "update");
	}

	return rv;
}

static GPtrArray *__get_glists(MidgardObject *object) 
{
	GPtrArray *gparray = g_ptr_array_new();

	guint n_prop, i;
	GList *cols = NULL;
	GList *values = NULL;
	GValue pval = {0, };
	const gchar *colname;
	const gchar *classname = G_OBJECT_TYPE_NAME (object);

	GParamSpec **pspecs =
		g_object_class_list_properties(G_OBJECT_GET_CLASS(object), &n_prop);

	MidgardObjectClass *klass = MIDGARD_OBJECT_GET_CLASS(object);
		
	for (i = 0; i < n_prop; i++) {

		if (G_TYPE_FUNDAMENTAL (pspecs[i]->value_type) == G_TYPE_OBJECT)
			continue;

		colname = midgard_core_class_get_property_colname(
				MIDGARD_DBOBJECT_GET_CLASS(object), pspecs[i]->name);

		if (colname == NULL)
			continue;

		/* FIXME, we should have boolean(s) here , not g_str_equal */
		const gchar *pprop = 
			midgard_reflector_object_get_property_primary(classname);
		if (g_str_equal(pprop, pspecs[i]->name)) {
			if (pspecs[i]->value_type == G_TYPE_UINT
					|| pspecs[i]->value_type == G_TYPE_INT)
				continue;
		}

		g_value_init(&pval, pspecs[i]->value_type);		
		g_object_get_property(G_OBJECT(object), pspecs[i]->name, &pval);
		GValue *dval = g_new0(GValue, 1);

		if (G_VALUE_TYPE (&pval) == MGD_TYPE_TIMESTAMP) {

			g_value_init (dval, GDA_TYPE_TIMESTAMP);
			g_value_transform (&pval, dval);

		} else { 

			g_value_init(dval, pspecs[i]->value_type);
			g_value_copy((const GValue*) &pval, dval);
		}

		values = g_list_prepend(values, (gpointer) dval);
		cols = g_list_prepend(cols, (gpointer)colname);

		g_value_unset(&pval);
	}

	g_free(pspecs);

	if (MGD_DBCLASS_METADATA_CLASS (MIDGARD_DBOBJECT_CLASS (klass))) {

		n_prop = 0;
		MidgardMetadata *mdata = MGD_DBOBJECT_METADATA (object);

		pspecs = g_object_class_list_properties(G_OBJECT_GET_CLASS(mdata), &n_prop);

		for(i = 0; i < n_prop; i++) {
		
			colname = midgard_core_class_get_property_colname(
					MIDGARD_DBOBJECT_GET_CLASS(mdata), pspecs[i]->name);

			if (!colname)
				continue;

			g_value_init(&pval, pspecs[i]->value_type);		
			g_object_get_property(G_OBJECT(mdata), pspecs[i]->name, &pval);
			GValue *dval = g_new0(GValue, 1);

			if (G_VALUE_TYPE (&pval) == MGD_TYPE_TIMESTAMP) {
	
				g_value_init (dval, GDA_TYPE_TIMESTAMP);
				g_value_transform (&pval, dval);
		
			} else { 
				
				g_value_init(dval, pspecs[i]->value_type);
				g_value_copy((const GValue*) &pval, dval);
			}

			values = g_list_prepend(values, (gpointer) dval);
			
			cols = g_list_prepend(cols, (gpointer)colname);			
			g_value_unset(&pval);
		}

		g_free (pspecs);
	}

	g_ptr_array_add (gparray, (gpointer) cols);
	g_ptr_array_add (gparray, (gpointer) values);
		
	return gparray;
}

gint 
__insert_or_update_records(MidgardObject *object, const gchar *tablename,  
				gint query_type, const gchar *where)
{
	GPtrArray *gparray;
	GList *l, *values;
	gint inserted = -1;
	
	/* INSERT object's record */
	gparray = __get_glists(object);

	inserted =
		midgard_core_query_insert_records(
				MGD_OBJECT_CNC (object), tablename,
				(GList*) g_ptr_array_index (gparray, 0),
				(GList*) g_ptr_array_index (gparray, 1),
				query_type, where);

	g_list_free((GList*) g_ptr_array_index (gparray, 0));
	
	values = (GList*) g_ptr_array_index (gparray, 1);
	for(l = values; l != NULL; l = l->next){
		g_value_unset((GValue *) l->data);
		g_free((GValue *) l->data);
	}
	g_list_free(values);
	g_ptr_array_free (gparray, TRUE);

	return inserted;
}

#define _CLEAR_OBJECT_GUID(object) \
	g_free ((gchar *)MGD_OBJECT_GUID(object)); \
	MGD_OBJECT_GUID(object) = NULL;

/* Create object's data in storage */
gboolean _midgard_object_create (	MidgardObject *object,	
					const gchar *create_guid,
					_ObjectActionUpdate replicate)
{
	GString *query;
	const gchar *tablename;
	MidgardConnection *mgd = MGD_OBJECT_CNC (object);	
	gint inserted;

	if (MIDGARD_DBOBJECT (object)->dbpriv->storage_data == NULL)
		return FALSE;

	/* Handle pure create call. If replicate is OBJECT_UPDATE_IMPORTED, then, 
	 * object has guid already set, which is valid for unserialized object */
	if (replicate == OBJECT_UPDATE_NONE 
			&& midgard_is_guid (MGD_OBJECT_GUID (object)) 
			&& MGD_OBJECT_IN_STORAGE (object)) {
		midgard_set_error(mgd,
				MGD_GENERIC_ERROR,
				MGD_ERR_DUPLICATE,
				"Object already created.");
		return FALSE;
	}

	/* Create object's guid, only if it's not already set */
	gchar *guid = NULL;
	if (MGD_OBJECT_GUID (object) == NULL) {
		
		if (create_guid == NULL)
			guid = midgard_guid_new(MGD_OBJECT_CNC (object));
		else
			guid = g_strdup(create_guid);
		
		MGD_OBJECT_GUID (object) = guid;
	}

	/* check if object is valid */
	if (!midgard_core_object_is_valid(object)) {
		_CLEAR_OBJECT_GUID (object);
		return FALSE;
	}
	
	MIDGARD_ERRNO_SET (MGD_OBJECT_CNC (object), MGD_ERR_OK);	

	/* Disable person check */
	/*
	if (object->person == NULL) {
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_ACCESS_DENIED);
		return FALSE;
	}
	*/

	tablename = midgard_core_class_get_table(MIDGARD_DBOBJECT_GET_CLASS(object));	

	/* FIXME , move this to object_is_valid */
	if (tablename == NULL) {
		/* Object has no storage defined. Return FALSE as there is nothing to create */
		g_warning ("Object '%s' has no table or storage defined!",
				G_OBJECT_TYPE_NAME(object));    
		MIDGARD_ERRNO_SET (MGD_OBJECT_CNC (object), MGD_ERR_NOT_EXISTS);    
		return FALSE;  
	}

	/* If Object has upfield set , check if property which points to up is set and has some value. 
	 * In other case, create Object and do INSERT, 
	 * as object is not "treeable" and may be created without 
	 * any parent object(typical for parameters or attachments).
	 */ 
	gint is_dup = _object_in_tree(object);
	if (is_dup == OBJECT_IN_TREE_DUPLICATE) {
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_DUPLICATE);
		_CLEAR_OBJECT_GUID (object);
		return FALSE;
	}
		
	if (!midgard_quota_create(object)) {
		_CLEAR_OBJECT_GUID (object);
		return FALSE;
	}	

	inserted = midgard_core_query_create_dbobject_record (MIDGARD_DBOBJECT (object));

	if (inserted == -1) {
		_CLEAR_OBJECT_GUID (object);
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_INTERNAL);
		return FALSE;
	}

	/* Always! get ID of newly created object */
	/* See: http://mail.gnome.org/archives/gnome-db-list/2007-April/msg00020.html */
	query = g_string_new("");
	g_string_append_printf(query, "guid = '%s' ", MGD_OBJECT_GUID (object));
	
	GValue *idval =
		midgard_core_query_get_field_value(
				mgd, "id", tablename,
				(const gchar*) query->str);
	g_string_free(query, TRUE);
	
	guint new_id = 0;
	if (G_IS_VALUE(idval)) {

		if (G_VALUE_TYPE(idval) == G_TYPE_UINT)
			new_id = g_value_get_uint(idval);
		else if (G_VALUE_TYPE(idval) == G_TYPE_INT)
			new_id = g_value_get_int(idval);
		else 
			g_warning("Unexpected type for primary key (%s)", G_VALUE_TYPE_NAME(idval));
		
		g_value_unset(idval);
		g_free(idval);
	}

	GParamSpec *pspec = 
		g_object_class_find_property(G_OBJECT_GET_CLASS(object), "id");
	
	if (pspec) {		

		if (new_id == 0) {

			g_critical("Newly created %s object's id is 0! ( object's guid - %s )",
					G_OBJECT_TYPE_NAME(G_OBJECT(object)),
					MGD_OBJECT_GUID (object));
			MIDGARD_ERRNO_SET (MGD_OBJECT_CNC (object), MGD_ERR_INTERNAL);    
			return FALSE;

		} else {

			g_object_set(G_OBJECT(object), "id", new_id, NULL);
		}
	}

	/* INSERT repligard's record */
	if (MGD_CNC_REPLICATION (mgd)) {
		query = g_string_new("INSERT INTO repligard ");
		g_string_append_printf(query,
				"(guid, typename, object_action) "
				"VALUES ('%s', '%s', %d)",
				MGD_OBJECT_GUID (object), G_OBJECT_TYPE_NAME(object), 
				MGD_OBJECT_ACTION_CREATE);
	
		midgard_core_query_execute(MGD_OBJECT_CNC (object), query->str, FALSE);
		g_string_free(query, TRUE);
	}

	switch(replicate){
		
		case OBJECT_UPDATE_NONE:
			g_signal_emit(object, MIDGARD_OBJECT_GET_CLASS(object)->signal_action_created, 0);
			break;
		
		case OBJECT_UPDATE_IMPORTED:
			g_signal_emit(object, MIDGARD_OBJECT_GET_CLASS(object)->signal_action_imported, 0);
			break;

		default:
			/* do nothing */
			break;
	}

	MGD_OBJECT_IN_STORAGE (object) = TRUE;

	return TRUE;
}

/**
 * midgard_object_create:
 * @object: #MidgardObject instance
 *
 * Creates new database record(s) for object.
 *
 * Internally such properties are set (overwritten):
 * <itemizedlist>
 * <listitem><para>
 * guid (if not set by root)
 * </para></listitem>
 * <listitem><para>
 * id (if set as primary property)
 * </para></listitem>
 * </itemizedlist> 
 * 
 * Metadata overwritten properties:
 * <itemizedlist>
 * <listitem><para>
 * creator
 * </para></listitem>
 * <listitem><para>
 * created
 * </para></listitem>
 * <listitem><para>
 * revisor
 * </para></listitem>
 * <listitem><para>
 * revised
 * </para></listitem>
 * <listitem><para>
 * revision
 * </para></listitem>
 * <listitem><para>
 * published ( set only, if not defined by user ) 
 * </para></listitem>
 * </itemizedlist> 
 * 
 * Cases to return %FALSE:
 * <itemizedlist>
 * <listitem><para>
 * Property registered with #MGD_TYPE_GUID doesn't hold valid guid ( MGD_ERR_INVALID_PROPERTY_VALUE ) 
 * </para></listitem>
 * <listitem><para>
 * Object's class is registered with tree facilities and there is already object with the same name in midgard tree. ( MGD_ERR_DUPLICATE ) 
 * </para></listitem>
 * <listitem><para>
 * Object has guid property set already. ( MGD_ERR_DUPLICATE ) 
 * </para></listitem>
 * <listitem><para>
 * Quota is activated and its limit is reached ( MGD_ERR_QUOTA ) 
 * </para></listitem>
 * <listitem><para>
 * Unspecified internal error ( MGD_ERR_INTERNAL ) 
 * </para></listitem>
 * </itemizedlist> 
 * 
 * Returns: %TRUE on success, %FALSE otherwise
 */
gboolean midgard_object_create(MidgardObject *object)
{
	g_assert(object != NULL);

	g_signal_emit(object, MIDGARD_OBJECT_GET_CLASS(object)->signal_action_create, 0);
	gboolean rv =  _midgard_object_create(object, NULL, OBJECT_UPDATE_NONE);	

	if (rv) {
		__dbus_send(object, "create");
	}

	return rv;
}

void _object_copy_properties(GObject *src, GObject *dest)
{
	g_assert(src != NULL && dest != NULL);

	guint nprop, i;
	GValue pval = {0, };	
	GParamSpec **props = g_object_class_list_properties(
			G_OBJECT_GET_CLASS(src),
			&nprop);

	/* guid property are set directly */
	if (MIDGARD_IS_OBJECT(src) && MIDGARD_IS_OBJECT(dest)) {
		MIDGARD_DBOBJECT(dest)->dbpriv->guid =
			g_strdup(MIDGARD_DBOBJECT(src)->dbpriv->guid);
	}
	
	for(i = 2; i <+ nprop; i++){		
		
		g_value_init(&pval, props[i]->value_type);	

		if (props[i]->value_type == G_TYPE_OBJECT) {			
		
			g_object_get_property(src, props[i]->name, &pval);
			
			GObject *nm = g_value_get_object(&pval);

			if (MIDGARD_IS_METADATA(nm)) {

				MidgardMetadata *new_metadata = 
					midgard_core_metadata_copy(MIDGARD_METADATA(nm));
				g_object_unref(nm);
				MGD_DBOBJECT_METADATA (dest) = new_metadata;
			}

		} else {

			g_object_get_property(G_OBJECT(src),
					props[i]->name,
					&pval);
			g_object_set_property(G_OBJECT(dest),
					props[i]->name,
					&pval);
		}

		g_value_unset(&pval);
	}

	g_free(props);
}

/**
 * midgard_object_get_by_id:
 * @object: #MidgardObject instance
 * @id: object's integer identifier
 *
 * Fetch object's record(s) from database using 'id' property.
 * 
 * This is common practice to use 'id' property with integer type when table's
 * id column stores unique, primary key value which identifies object and its record(s).
 * However primary property with integer type is freely defined by user.
 * 
 * MidgardObject object instance must be created with midgard_object_new function.
 * When midgard connection handler is not associated with object instance, 
 * application is terminated with 'assertion fails' error message being logged. 
 *
 * Object instance created with this function should be freed using #g_object_unref.
 * 
 * Cases to return %FALSE:
 * <itemizedlist>
 * <listitem><para>
 * There's no 'id' primary property registered for object's class ( MGD_ERR_INTERNAL )
 * </para></listitem>
 * <listitem><para>
 * Object's record can not be fetched from database ( MGD_ERR_NOT_EXISTS ) 
 * </para></listitem>
 * <listitem><para>
 * unspecified internal error ( MGD_ERR_INTERNAL )   
 * </para></listitem>
 * </itemizedlist> 
 * 
 * Returns: %TRUE if object is successfully fetched from database, %FALSE otherwise. 
 */ 
gboolean midgard_object_get_by_id(MidgardObject *object, guint id)
{
	g_assert(object != NULL);
	g_assert(MGD_OBJECT_CNC (object) != NULL);

	/* Reset errno */
	MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_OK);

	GParamSpec *prop;
	MidgardObjectClass *klass = MIDGARD_OBJECT_GET_CLASS(object);
	
	/* Find "primary" property , just in case of fatal error */
	prop = g_object_class_find_property((GObjectClass*)klass, "id");

	if (prop == NULL){
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_INTERNAL);
		g_warning("Primary property id not found for object's klass '%s'",
				G_OBJECT_TYPE_NAME(object));
		return FALSE;
	}

	/* Stop when property is not uint type */
	g_assert((prop->value_type == G_TYPE_UINT));
	
	/* Initialize QB */
	MidgardQueryBuilder *builder =
		midgard_query_builder_new(MGD_OBJECT_CNC (object), 
				G_OBJECT_TYPE_NAME(object));
	
	if (!builder) {
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_INTERNAL);
		g_warning("Invalid query builder configuration (%s)",
				G_OBJECT_TYPE_NAME(object));
		return FALSE;
	}

	/* Get primary's property value */
	GValue pval = {0,};
	g_value_init(&pval,prop->value_type);	
	g_value_set_uint(&pval, id);
	if (midgard_query_builder_add_constraint(builder,
				"id",
				"=", &pval)) {

		GList *olist =
			midgard_core_qb_set_object_from_query(builder, MQB_SELECT_OBJECT, &object);
		g_value_unset(&pval);
		g_object_unref(G_OBJECT(builder));
		
		if (!olist) {
			
			MIDGARD_ERRNO_SET(MGD_OBJECT_CNC(object), MGD_ERR_NOT_EXISTS);
			return FALSE;
		}
		
		g_list_free(olist);
		
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC(object), MGD_ERR_OK);
		__dbus_send(object, "get");
		return TRUE;
	}
	
	return FALSE;
}

static gboolean 
_object_create_storage(MidgardConnection *mgd, MidgardDBObjectClass *klass)
{
	g_return_val_if_fail(mgd != NULL, FALSE);
	g_return_val_if_fail(klass != NULL, FALSE);

	return midgard_core_query_create_class_storage(mgd, klass);
}

static gboolean 
_object_update_storage(MidgardConnection *mgd, MidgardDBObjectClass *klass)
{
	g_return_val_if_fail(mgd != NULL, FALSE);
	g_return_val_if_fail(klass != NULL, FALSE);

	return midgard_core_query_update_class_storage(mgd, klass);
}

static gboolean 
_object_storage_exists(MidgardConnection *mgd, MidgardDBObjectClass *klass)
{
	g_return_val_if_fail(mgd != NULL, FALSE);
	g_return_val_if_fail(klass != NULL, FALSE);

	const gchar *tablename =
		midgard_core_class_get_table(klass);

	/* NULL storage, return TRUE, it's OK */
	if (!tablename)
		return TRUE;
        
 	return midgard_core_table_exists(mgd, tablename);
}

static gboolean 
_object_delete_storage(MidgardConnection *mgd, MidgardDBObjectClass *klass)
{
	g_return_val_if_fail(mgd != NULL, FALSE);
	g_return_val_if_fail(klass != NULL, FALSE);

	g_warning("%s delete storage not implemented", G_OBJECT_CLASS_NAME(klass));
	return FALSE;
}

/* Initialize class. 
 * Properties setting follow data in class_data.
 */ 
static void
__mgdschema_class_init(gpointer g_class, gpointer class_data)
{
	MgdSchemaTypeAttr *data = (MgdSchemaTypeAttr *) class_data;

	GObjectClass *gobject_class = G_OBJECT_CLASS(g_class);
	MidgardObjectClass *mklass = (MidgardObjectClass *) g_class;
	MidgardDBObjectClass *dbklass = MIDGARD_DBOBJECT_CLASS (g_class);
	guint idx;
	
	__mgdschema_parent_class = g_type_class_peek_parent (g_class);

	gobject_class->set_property = __midgard_object_set_property;
	gobject_class->get_property = __midgard_object_get_property;
	gobject_class->finalize = __mgdschema_object_finalize;
	gobject_class->constructor = __mgdschema_object_constructor;
        gobject_class->dispose = __mgdschema_object_dispose;

	mklass->get_connection = MIDGARD_OBJECT_CLASS(mklass)->get_connection;

	if (mklass) {

		mklass->priv = g_new (MidgardObjectClassPrivate, 1);
		dbklass->dbpriv = g_new (MidgardDBObjectPrivate, 1);	

		/* Check metadata. No support for user declared one yet. */
		if (data->metadata_class_name == NULL) 
			dbklass->dbpriv->has_metadata = FALSE;
		else 
			dbklass->dbpriv->has_metadata = TRUE;

		dbklass->dbpriv->storage_data = data;
		dbklass->dbpriv->set_from_sql = NULL;
		dbklass->dbpriv->__set_from_sql = NULL;
		dbklass->dbpriv->set_from_xml_node = NULL;

		dbklass->dbpriv->create_storage = _object_create_storage;
		dbklass->dbpriv->update_storage = _object_update_storage;
		dbklass->dbpriv->storage_exists = _object_storage_exists;
		dbklass->dbpriv->delete_storage = _object_delete_storage;
		dbklass->dbpriv->add_fields_to_select_statement = MIDGARD_DBOBJECT_CLASS (__mgdschema_parent_class)->dbpriv->add_fields_to_select_statement;
		dbklass->dbpriv->get_property = MIDGARD_DBOBJECT_CLASS (__mgdschema_parent_class)->dbpriv->get_property;
		dbklass->dbpriv->set_from_data_model = MIDGARD_DBOBJECT_CLASS (__mgdschema_parent_class)->dbpriv->set_from_data_model;
		dbklass->dbpriv->set_statement_insert = MIDGARD_DBOBJECT_CLASS (__mgdschema_parent_class)->dbpriv->set_statement_insert;
	}	

	if (G_OBJECT_CLASS_TYPE(g_class) != MIDGARD_TYPE_OBJECT) {
		__add_core_properties(dbklass->dbpriv->storage_data);
	}

	g_type_class_add_private (g_class, sizeof(MgdSchemaTypeAttr));
	
	/* List parent class properties so we can set current class base_index */
	guint n_prop;
	GParamSpec **pspecs =
		g_object_class_list_properties (g_type_class_peek_parent(g_class), &n_prop);
	g_free(pspecs);

	if (data)
		data->properties = 
			g_malloc(sizeof(MgdSchemaPropertyAttr*)
					*(data->num_properties+1));
	
	data->base_index = n_prop;

	/* Note, that we start numbering from 1 , not from 0. property_id must be > 0 */
	for (idx = 1; idx <= data->num_properties; idx++) {
		/*gchar *pname = data->params[idx-1]->name;
		 g_print ("Installing property id %d :: %s.%s \n",
				idx, G_OBJECT_CLASS_NAME (G_OBJECT_CLASS (g_class)), pname); */
		g_object_class_install_property(
				gobject_class, 
				data->base_index + idx , 
				data->params[idx-1]);
	}

	/* Reset params spec */
	g_free (data->params);
	data->params = NULL;

	/* Initialize persistent statement */
	dbklass->dbpriv->set_statement_insert (dbklass);
}

static void
__add_core_properties (MgdSchemaTypeAttr *type_attr)
{
	/* GUID */
	MgdSchemaPropertyAttr *prop_attr = midgard_core_schema_type_property_attr_new();
	prop_attr->name = g_strdup ("guid");
	prop_attr->gtype = MGD_TYPE_GUID;
	prop_attr->field = g_strdup ("guid");
	prop_attr->table = g_strdup (type_attr->table);
	prop_attr->tablefield = g_strjoin (".", type_attr->table, "guid", NULL);
	g_hash_table_insert (type_attr->prophash, g_strdup((gchar *)"guid"), prop_attr);
	type_attr->_properties_list = g_slist_prepend (type_attr->_properties_list, (gpointer)prop_attr->name);
	
	return;
}

static GObject *
__mgdschema_object_constructor (GType type,
		guint n_construct_properties,
		GObjectConstructParam *construct_properties)
{	
	GObject *object = (GObject *)
		G_OBJECT_CLASS (__mgdschema_parent_class)->constructor (type,
				n_construct_properties,
				construct_properties);
	
	/* Workaround to set default values for every property.
	 * I have no idea why we get 0 n_construct_properties and null GObjectConstructParam */
	//GObjectClass *g_class = g_type_class_peek (type);
	GObjectClass *g_class = G_OBJECT_GET_CLASS (object);
	guint n_prop, i;
	GParamSpec **pspecs = g_object_class_list_properties(g_class, &n_prop);

	MgdSchemaTypeAttr *type_attr = MIDGARD_DBOBJECT_CLASS (g_class)->dbpriv->storage_data;
	for (i = 0; i < n_prop ; i++) {
	
		MgdSchemaPropertyAttr *prop_attr = 
			midgard_core_class_get_property_attr (MIDGARD_DBOBJECT_CLASS(g_class), pspecs[i]->name);

		if (prop_attr == NULL 
				|| prop_attr->default_value == NULL 
				|| !G_IS_VALUE(prop_attr->default_value)) 
			continue;

		g_object_set_property(object, pspecs[i]->name, prop_attr->default_value);
	}	

	g_free(pspecs);

	if (MGD_TYPE_ATTR_METADATA_CLASS (type_attr)) {
		MGD_DBOBJECT_METADATA (object) = midgard_metadata_new (MIDGARD_DBOBJECT (object));
		/* Add weak reference */
		g_object_add_weak_pointer (object, (gpointer) MGD_DBOBJECT_METADATA (object));
	}
	
	return object;
}

GType
midgard_type_register (MgdSchemaTypeAttr *type_data, GType parent_type)
{
	gchar *classname = type_data->name;

	GType class_type = g_type_from_name (classname);

        if (class_type) 
                return class_type;

        {
                GTypeInfo *midgard_type_info = g_new0 (GTypeInfo, 1);

		/* FIXME, does it make sense? */
		if (type_data == NULL)
			type_data = g_new (MgdSchemaTypeAttr, 1);

                /* our own class size is 0 but it should include space for a parent, therefore add it */
                midgard_type_info->class_size = sizeof (MidgardObjectClass);
                midgard_type_info->base_init = NULL;
                midgard_type_info->base_finalize = NULL;
                midgard_type_info->class_init  = __mgdschema_class_init;
                midgard_type_info->class_finalize  = NULL;
                midgard_type_info->class_data = type_data;
                /* our own instance size is 0 but it should include space for a parent,
                 * therefore add it */
                midgard_type_info->instance_size = sizeof (MidgardObject);
                midgard_type_info->n_preallocs = 0;
                midgard_type_info->instance_init = __midgard_object_instance_init;
                midgard_type_info->value_table = NULL;
                
		GType type = g_type_register_static (MIDGARD_TYPE_OBJECT, classname, midgard_type_info, 0);
               
	        /* FIXME, MidgardAttachment should be registered directly in core, instead of schema */	
		if (g_str_equal (classname, "midgard_attachment"))
			_midgard_attachment_type = type;

                g_free (midgard_type_info);
                return type;   
        }                      
}

/* Workaround for midgard_attachment created as MgdSchema class and type */
GType midgard_attachment_get_type(void)
{
	g_assert(_midgard_attachment_type != 0);

	return _midgard_attachment_type;
}

/* MIDGARD_OBJECT */

static GObjectClass *__midgard_object_parent_class = NULL;

static GObject *
__midgard_object_constructor (GType type,
		guint n_construct_properties,
		GObjectConstructParam *construct_properties)
{
	GObject *object = 
		G_OBJECT_CLASS (__midgard_object_parent_class)->constructor (type,
				n_construct_properties,
				construct_properties);

	MIDGARD_DBOBJECT(object)->dbpriv->storage_data =
		MIDGARD_DBOBJECT_GET_CLASS(object)->dbpriv->storage_data;
	//MIDGARD_DBOBJECT (object)->dbpriv->metadata = MIDGARD_OBJECT (object)->metadata;

	MgdSchemaTypeAttr *priv =
		G_TYPE_INSTANCE_GET_PRIVATE ((GTypeInstance*)object, type, MgdSchemaTypeAttr);

	priv->base_index = 1;
	priv->num_properties = MIDGARD_DBOBJECT(object)->dbpriv->storage_data->class_nprop;
	
	/* Allocate properties storage for this instance  */
	priv->properties = 
		priv->num_properties ? g_new0 (MgdSchemaPropertyAttr*, priv->num_properties) : NULL;
	
	if (priv->properties) {
		
		guint idx;
		for (idx = 0; idx < priv->num_properties; idx++) {
			priv->properties[idx] = g_new0 (MgdSchemaPropertyAttr, 1);
		}
	}
	
	return object;
}

static void 
__midgard_object_dispose (GObject *object)
{
	__midgard_object_parent_class->dispose (object);
}

static void 
__midgard_object_finalize (GObject *object)
{
	__midgard_object_parent_class->finalize (object);
}

static void
__midgard_object_class_init (MidgardObjectClass *klass, gpointer g_class_data)
{
	GObjectClass *g_class = G_OBJECT_CLASS (klass);
	__midgard_object_parent_class = g_type_class_peek_parent (klass);


	MidgardDBObjectClass *dbklass = MIDGARD_DBOBJECT_CLASS (klass);
	
	dbklass->dbpriv->create_storage = NULL;
	dbklass->dbpriv->update_storage = NULL;
	dbklass->dbpriv->delete_storage = NULL;
	dbklass->dbpriv->storage_exists = NULL;

	g_class->constructor = __midgard_object_constructor;
	g_class->dispose = __midgard_object_dispose;
	g_class->finalize = __midgard_object_finalize;
	g_class->set_property = __midgard_object_set_property;
	g_class->get_property = __midgard_object_get_property;

	MgdSchemaTypeAttr *data = (MgdSchemaTypeAttr *) g_class_data;
	guint idx;

	if (data) {
		data->properties = g_malloc(sizeof(MgdSchemaPropertyAttr*) * (data->num_properties+1));
		
		/* Note, that we start numbering from 1 , not from 0. property_id must be > 0 */
		for (idx = 1; idx <= data->num_properties; idx++) {
			/*g_warning("Installing property id %d :: %s",
					idx, data->params[idx-1]->name); */
			g_object_class_install_property (
					G_OBJECT_CLASS (klass), 
					data->base_index + idx , 
					data->params[idx-1]);
		}  
	}

	MidgardObjectClass *mklass = klass;

	mklass->get_connection = MIDGARD_DBOBJECT_CLASS(mklass)->get_connection;
	dbklass->dbpriv->add_fields_to_select_statement = MIDGARD_DBOBJECT_CLASS (__midgard_object_parent_class)->dbpriv->add_fields_to_select_statement;
	dbklass->dbpriv->get_property = MIDGARD_DBOBJECT_CLASS (__midgard_object_parent_class)->dbpriv->get_property;
	dbklass->dbpriv->set_from_data_model = MIDGARD_DBOBJECT_CLASS (__midgard_object_parent_class)->dbpriv->set_from_data_model;
	dbklass->dbpriv->set_statement_insert = MIDGARD_DBOBJECT_CLASS (__midgard_object_parent_class)->dbpriv->set_statement_insert;

	if (!signals_registered && mklass) {
		
		mklass->signal_action_create = 
			g_signal_new("action-create",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_create),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,
					G_TYPE_NONE,
					0);

		mklass->signal_action_create_hook =
			g_signal_new("action-create-hook",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_create_hook),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,
					G_TYPE_NONE,
					0);

		mklass->signal_action_created =
			g_signal_new("action-created",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_created),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);
	
		mklass->signal_action_update =
			g_signal_new("action-update",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,	
					G_STRUCT_OFFSET (MidgardObjectClass, action_update),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);
	
		mklass->signal_action_update_hook =
			g_signal_new("action-update-hook",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,	
					G_STRUCT_OFFSET (MidgardObjectClass, action_update_hook),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_updated =
			g_signal_new("action-updated",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_updated),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_delete =
			g_signal_new("action-delete",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_delete),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_delete_hook =
			g_signal_new("action-delete-hook",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_delete_hook),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_deleted =
			g_signal_new("action-deleted",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_deleted),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_purge =
			g_signal_new("action-purge",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_purge),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_purge_hook =
			g_signal_new("action-purge-hook",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_purge_hook),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_purged =
			g_signal_new("action-purged",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_purged),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_import =
			g_signal_new("action-import",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_import),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_import_hook =
			g_signal_new("action-import-hook",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_import_hook),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_imported =
			g_signal_new("action-imported",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_imported),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_export =
			g_signal_new("action-export",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_export),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_export_hook =
			g_signal_new("action-export-hook",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_export_hook),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_exported =
			g_signal_new("action-exported",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_exported),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_loaded =
			g_signal_new("action-loaded",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_loaded),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_loaded_hook =
			g_signal_new("action-loaded-hook",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_loaded_hook),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_approve =
			g_signal_new("action-approve",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_approve),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);
	
		mklass->signal_action_approve_hook =
			g_signal_new("action-approve_hook",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_approve_hook),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_approved =
			g_signal_new("action-approved",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_approved),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);
		
		mklass->signal_action_unapprove =
			g_signal_new("action-unapprove",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_unapprove),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_unapprove_hook =
			g_signal_new("action-unapprove_hook",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_unapprove_hook),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_unapproved =
			g_signal_new("action-unapproved",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_unapproved),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_lock =
			g_signal_new("action-lock",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_lock),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_lock_hook =
			g_signal_new("action-lock-hook",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_lock_hook),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_locked =
			g_signal_new("action-locked",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_locked),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_unlock =
			g_signal_new("action-unlock",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_unlock),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		mklass->signal_action_unlock_hook =
			g_signal_new("action-unlock-hook",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_unlock_hook),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);
		
		mklass->signal_action_unlocked =
			g_signal_new("action-unlocked",
					G_TYPE_FROM_CLASS(g_class),
					G_SIGNAL_ACTION,
					G_STRUCT_OFFSET (MidgardObjectClass, action_unlocked),
					NULL, /* accumulator */
					NULL, /* accu_data */
					g_cclosure_marshal_VOID__VOID,	
					G_TYPE_NONE,
					0);

		signals_registered = TRUE;
	}
}

GType midgard_object_get_type(void) 
{
        static GType type = 0;
        if (type == 0) {
                MgdSchemaTypeAttr *class_data = g_new(MgdSchemaTypeAttr, 1);
                class_data->params = _midgard_object_class_paramspec();
                class_data->base_index = 0;
                class_data->num_properties = 0;
                while (class_data->params[class_data->num_properties]) {
                        class_data->num_properties++;
                }

                GTypeInfo info = {
                        sizeof(MidgardObjectClass),
                        NULL,                       /* base_init */
                        NULL,                       /* base_finalize */
                        (GClassInitFunc) __midgard_object_class_init,   /* class_init */
                        NULL,                       /* class_finalize */
                        class_data,	                    /* class_data */
                        sizeof(MidgardObject),
                        0,                          /* n_preallocs */
                        NULL,        /* instance_init */
                };
                type = g_type_register_static (MIDGARD_TYPE_DBOBJECT, "MidgardObject", &info, G_TYPE_FLAG_ABSTRACT);
        }
        return type;
}

/**
 * midgard_object_new:
 * @mgd: #MidgardConnection handler
 * @name: name of the class 
 * @value: (allow-none): optional value which holds id or guid of an object 
 * 
 * Creates new MidgardObject object instance.
 *
 * This function is mandatory one for new midgard object initialization.
 * Unlike g_object_new ( which is typical function to create new GObject 
 * instance ), midgard_object_new initializes data which are accessible
 * internally by object instance itself or by object's class:
 *
 * - midgard connection handler is associated with object
 * 
 * Sitegroup value is returned from midgard connection handler and may 
 * be overwritten only by SG0 Midgard Administrator only when object
 * is created. Setting this property is forbidden when object is already
 * fetched from database. 
 *
 * Object's contructor tries to determine third optional parameter value.
 * If it's of #G_TYPE_STRING type , then midgard_is_guid() is called to check 
 * weather passed string is a guid , in any other case id property is used 
 * with #G_TYPE_UINT type. New "empty" instance is created (without fetching 
 * data from database) if @value parameter is explicitly set to NULL.
 * 
 * Any object instance created with this function should be freed using typical
 * #g_object_unref function.
 * 
 * Cases to return %NULL:
 * <itemizedlist>
 * <listitem><para>
 * @value holds string but it's not a valid guid
 * </para></listitem>
 * <listitem><para>
 * @value holds valid id or guid but object doesn't exists in database ( MGD_ERR_NOT_EXISTS )
 * </para></listitem>
 * <listitem><para>
 * unspecified internal error ( MGD_ERR_INTERNAL ) 
 * </para></listitem>
 * </itemizedlist> 
 * 
 * Returns: New #MidgardObject object or %NULL on failure 
 */ 
MidgardObject *
midgard_object_new (MidgardConnection *mgd, const gchar *name, GValue *value)
{
	g_return_val_if_fail (mgd != NULL, NULL);
	g_return_val_if_fail (name != NULL, NULL);

	MIDGARD_ERRNO_SET (mgd, MGD_ERR_OK);

	GType type = g_type_from_name (name);
	MidgardObject *self;
	const gchar *field = "id";	

	if (!type)
		return NULL;

	/* Empty object instance */
	if (value == NULL || (value && G_VALUE_TYPE (value) == G_TYPE_NONE)) {	
		goto return_empty_object;
	} else {
		
		/* We have to accept both integer and unsigned integer type.
		 * There might be no control over value passed on bindings level. */
		if (G_VALUE_TYPE(value) != G_TYPE_STRING) {
			if (G_VALUE_TYPE(value) != G_TYPE_UINT) {
				if (G_VALUE_TYPE(value) != G_TYPE_INT) {
					g_warning ("Expected value of string or integer type");
					return NULL;
				}
			}
		}

		if (G_VALUE_TYPE(value) == G_TYPE_UINT) {
			if (g_value_get_uint(value) == 0) {
				goto return_empty_object;
			}
		}	

		if (G_VALUE_TYPE(value) == G_TYPE_INT) {
			if (g_value_get_int(value) < 1) {
				goto return_empty_object;
			}
		}	

		if (G_VALUE_TYPE(value) == G_TYPE_STRING) {
			const gchar *guidval = g_value_get_string(value);
			field = "guid";
			if (!guidval || (guidval && !midgard_is_guid (guidval)))
					goto return_empty_object;
		}
		
		MidgardQueryBuilder *builder = midgard_query_builder_new(mgd, name);
		midgard_query_builder_add_constraint(builder, field, "=", value);
		guint n_objects;
		GObject **objects = midgard_query_builder_execute(builder, &n_objects);
		g_object_unref(builder);

		if (!objects) {
			MIDGARD_ERRNO_SET(mgd, MGD_ERR_NOT_EXISTS);				
			return NULL;
		}

		self = MIDGARD_OBJECT(objects[0]);
		g_free(objects);	

		if (!self) {
			MIDGARD_ERRNO_SET(mgd, MGD_ERR_INTERNAL);
			return NULL;
		}

		__dbus_send(self, "get");
		MIDGARD_DBOBJECT (self)->dbpriv->storage_data = MIDGARD_DBOBJECT_GET_CLASS(self)->dbpriv->storage_data;
		MGD_OBJECT_CNC (self) = mgd;	
		
		return self;
	}

return_empty_object:
	self = g_object_new(type, "connection", mgd, NULL);
	MGD_OBJECT_CNC (self) = mgd;

	return self;
}

/**
 * midgard_object_factory:
 * @mgd: #MidgardConnection instance
 * @name: #MidgardObject derived class name
 * @value: (allow-none): optional object identifier
 *
 * Static constructor, provided for language bindings, in which, 
 * midgard_object_new() can not be invoked explicitly.
 *
 * Since: 10.05.1
 */
MidgardObject *
midgard_object_factory (MidgardConnection *mgd, const gchar *name, GValue *value)
{
	return midgard_object_new (mgd, name, value);
}

/**
 * midgard_object_get_parent:
 * @self: #MidgardObject instance
 *
 * Fetch parent ( in midgard tree ) object.
 *
 * This function fetches "upper" object of the same type, and then parent 
 * object if object's up property holds empty value. 
 * Object is root object in tree when NULL is returned.
 *
 * Returns: new #MidgardObject object instance or %NULL 
 */ 
MidgardObject *midgard_object_get_parent(MidgardObject *self)
{
	MidgardObject *mobj = self;
        g_assert(mobj != NULL);

        MidgardObject *pobj = NULL;
        const gchar *pcstring;
        guint puint = 0;
        gint pint = 0;
        GParamSpec *fprop = NULL;
        GValue pval = {0,};
        gboolean ret_object = FALSE;
        const gchar *parent_class_name = NULL;
        MidgardConnection *mgd = MGD_OBJECT_CNC (self);
        MidgardObjectClass *klass = MIDGARD_OBJECT_GET_CLASS(mobj);
	const gchar *classname = G_OBJECT_TYPE_NAME (self);

        const gchar *property_up = midgard_reflector_object_get_property_up (classname);

        if (property_up) {

		fprop = g_object_class_find_property( G_OBJECT_GET_CLASS(mobj), property_up);
		MidgardReflectionProperty *mrp = midgard_reflection_property_new (MIDGARD_DBOBJECT_CLASS (klass));
		if (midgard_reflection_property_is_link (mrp, property_up)) {

			parent_class_name = midgard_reflection_property_get_link_name (mrp, property_up);
   
			if (parent_class_name)
				pobj = midgard_object_new (mgd, parent_class_name, NULL);

			g_object_unref (mrp);
		}

		if (!pobj)
			return NULL;
	}

	if (fprop) {
		
		g_value_init(&pval,fprop->value_type);
		g_object_get_property(G_OBJECT(mobj), 
				midgard_reflector_object_get_property_up(classname), &pval);
		
		switch(fprop->value_type) {
			
			case G_TYPE_STRING:
				
				if ((pcstring = g_value_get_string(&pval)) != NULL) {
					
					ret_object = TRUE;
					if (!midgard_object_get_by_guid(pobj, pcstring)) {
						g_object_unref(pobj);
						pobj = NULL;
					}
				}
				break;

			case G_TYPE_UINT:
				
				if ((puint = g_value_get_uint(&pval))) {

					ret_object = TRUE;
					if (!midgard_object_get_by_id(pobj, puint)) {
						g_object_unref(pobj);
						pobj = NULL;
					}
				}				
				break;

				
			case G_TYPE_INT:

				if ((pint = g_value_get_int(&pval))) {

					ret_object = TRUE;
					if (!midgard_object_get_by_id(pobj, pint)) {
						g_object_unref(pobj);
						pobj = NULL;
					}
				}
				break;
		}
		
		g_value_unset(&pval);

		if (ret_object) {
			return pobj;
		} else {
			g_object_unref(pobj);
			pobj = NULL;
		}
	}
        
        /* I do make almost the same for property_parent, because I want to 
	 * avoid plenty of warnings after G_VALUE_HOLDS which could be used 
	 * with  value returned for mobj->priv->storage_data->tree->property_up 
	 */ 

	if (midgard_reflector_object_get_property_parent(classname) == NULL)
		return NULL;

	parent_class_name = midgard_schema_object_tree_get_parent_name (self);
	if (!parent_class_name)
		return NULL;

	pobj =  midgard_object_new(MGD_OBJECT_CNC (self) , MIDGARD_DBOBJECT (mobj)->dbpriv->storage_data->parent, NULL);
	
	if (pobj == NULL)
		return NULL;
	
	fprop = g_object_class_find_property(
			G_OBJECT_GET_CLASS(mobj),
			midgard_reflector_object_get_property_parent(classname));

	if (!fprop)
		return NULL;
       
        g_value_init(&pval,fprop->value_type);
        g_object_get_property(G_OBJECT(mobj), 
			midgard_reflector_object_get_property_parent(classname) , &pval);

        switch(fprop->value_type) {
            
            case G_TYPE_STRING:
		    
		    if ((pcstring = g_value_get_string(&pval)) != NULL) {
			    
			    if (!midgard_object_get_by_guid(pobj, pcstring)) {
				    g_object_unref(pobj);
				    pobj = NULL;
			    }
		    }
		    break;

            case G_TYPE_UINT:
		    
		    if ((puint = g_value_get_uint(&pval))) {

			    if (!midgard_object_get_by_id(pobj, puint)) {
				    g_object_unref(pobj);
				    pobj = NULL;
			    }
		    }
		    break;

	    case G_TYPE_INT:
		    
		    if ((pint = g_value_get_int(&pval))) {

			    if (!midgard_object_get_by_id(pobj, pint)) {
				    g_object_unref(pobj);
				    pobj = NULL;
			    }
		    }
		    break;


        }
        
        g_value_unset(&pval);
        
        return pobj;
}

/**
 * midgard_object_get_by_guid:
 * @object: #MidgardObject instance
 * @guid: string value which should identify object 
 * 
 * Fetch object's record(s) from database using 'guid' property constraint.
 *
 * MidgardObject object instance must be created with midgard_object_new function.
 * When midgard connection handler is not associated with object instance, 
 * application is terminated with 'assertion fails' error message being logged. 
 * 
 * Object instance created with this function should be freed using g_object_unref.
 *  
 * Cases to return %FALSE:
 * <itemizedlist>
 * <listitem><para>
 * Object's record can not be fetched from database ( MGD_ERR_NOT_EXISTS ) 
 * </para></listitem>
 * <listitem><para>
 * unspecified internal error ( MGD_ERR_INTERNAL ) 
 * </para></listitem>
 * </itemizedlist> 
 * 
 * Returns: %TRUE if object is successfully fetched from database, %FALSE otherwise.
 */ 
gboolean midgard_object_get_by_guid(MidgardObject *object, const gchar *guid)
{
	MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_OK);
	
	if (MIDGARD_DBOBJECT (object)->dbpriv->storage_data == NULL) {
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_NOT_EXISTS);
		return FALSE; 
	}
	
	if (!midgard_core_class_get_table(MIDGARD_DBOBJECT_GET_CLASS(object))){
		/* Object has no storage defined. Return NULL as there is nothing to get */
		g_warning("Object '%s' has no table or storage defined!", 
				G_OBJECT_TYPE_NAME(object));
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_NOT_EXISTS);
		return FALSE;
	}
	
	MidgardQueryBuilder *builder =
		midgard_query_builder_new(MGD_OBJECT_CNC (object),
				G_OBJECT_TYPE_NAME(object));
	if (!builder) {
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_INTERNAL);
		return FALSE;
	}

	GValue pval = {0,};
	g_value_init(&pval,G_TYPE_STRING);
	g_value_set_string(&pval, guid);	
	if (midgard_query_builder_add_constraint(builder,
				"guid",
				"=", &pval)) {
	
		GList *olist =
			midgard_core_qb_set_object_from_query(builder, MQB_SELECT_OBJECT, &object);
		
		g_value_unset(&pval);
		g_object_unref(G_OBJECT(builder));
		
		if (!olist) {
			
			MIDGARD_ERRNO_SET(MGD_OBJECT_CNC(object), MGD_ERR_NOT_EXISTS);
			return FALSE;
		}

		g_list_free(olist);
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC(object), MGD_ERR_OK);
		__dbus_send(object, "get");
		return TRUE;

	} 
	
	MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_NOT_EXISTS);
	return FALSE;	
}


gboolean _midgard_object_delete(MidgardObject *object, gboolean check_dependents)
{
	g_assert(object);
	
	MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_OK);
	g_signal_emit(object, MIDGARD_OBJECT_GET_CLASS(object)->signal_action_delete, 0);		
	
	if (MIDGARD_DBOBJECT (object)->dbpriv->storage_data == NULL) {
		g_warning("No schema attributes for class %s",
				G_OBJECT_TYPE_NAME(object));
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_INTERNAL);
		return FALSE;
	}

	const gchar *table =
		midgard_core_class_get_table(MIDGARD_DBOBJECT_GET_CLASS(object));
	
	if (table  == NULL) {
		/* Object has no storage defined. Return FALSE as there is nothing to update */
		g_warning("Object '%s' has no table defined!", G_OBJECT_TYPE_NAME(object));
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_OBJECT_NO_STORAGE);
		return FALSE;
	}

	const gchar *guid = MGD_OBJECT_GUID(object);
	
	if (!guid){
		midgard_set_error(MGD_OBJECT_CNC(object),
				MGD_GENERIC_ERROR,
				MGD_ERR_INVALID_PROPERTY_VALUE, 
				"Guid property is NULL. ");
		return FALSE;
	}
	
	if (check_dependents && midgard_object_has_dependents(object)) {		
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC(object), MGD_ERR_HAS_DEPENDANTS);
		return FALSE;
	}
	
	GString *sql = g_string_new("UPDATE ");
	g_string_append_printf(sql,
			"%s SET ",
			table);
	
	gchar *person_guid = "";

	MidgardConnection *mgd = MGD_OBJECT_CNC (object);
	MidgardObject *person = MGD_CNC_PERSON (mgd);
	if (person)
		person_guid = (gchar *)MGD_OBJECT_GUID(person);
	
	GValue tval = {0, };
	g_value_init (&tval, MGD_TYPE_TIMESTAMP);
	midgard_timestamp_set_current_time(&tval);
	gchar *timeupdated = midgard_timestamp_get_string_from_value (&tval);
	midgard_core_metadata_increase_revision (MGD_DBOBJECT_METADATA (object));
	g_string_append_printf(sql,
			"metadata_revisor='%s', metadata_revised='%s',"
			"metadata_revision=%d, "
			"metadata_deleted=1 ",
			person_guid, timeupdated, 
			MGD_DBOBJECT_METADATA (object)->priv->revision);

	g_string_append_printf(sql, " WHERE guid = '%s' ",  MGD_OBJECT_GUID(object));
        gint qr = midgard_core_query_execute(MGD_OBJECT_CNC (object), sql->str, FALSE);

	g_string_free(sql, TRUE);
	g_free (timeupdated);

	MidgardMetadata *metadata = MGD_DBOBJECT_METADATA (object);

	if (qr == 0) {
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_INTERNAL);
		g_value_unset(&tval);
		
		if (metadata)
			metadata->priv->revision--;

		return FALSE;

	} 

	if (MGD_CNC_REPLICATION (mgd)) {
		sql = g_string_new("UPDATE repligard SET ");
		g_string_append_printf(sql,
				"object_action = %d "
				"WHERE guid = '%s' AND typename = '%s' ",
				MGD_OBJECT_ACTION_DELETE,
				guid,
				G_OBJECT_TYPE_NAME(G_OBJECT(object)));
		
		midgard_core_query_execute(MGD_OBJECT_CNC(object), sql->str, FALSE);
		g_string_free(sql, TRUE);
	}

	/* Set metadata properties */
	if (metadata) {
		midgard_core_metadata_set_revised (metadata, &tval);
		GValue gval = {0, };
		g_value_init (&gval, G_TYPE_STRING);
		g_value_set_string (&gval, person_guid);
		midgard_core_metadata_set_revisor (metadata, &gval);
		g_value_unset (&gval);
		metadata->priv->deleted = TRUE;
	}

	g_value_unset(&tval);

	g_signal_emit(object, MIDGARD_OBJECT_GET_CLASS(object)->signal_action_deleted, 0);
	__dbus_send(object, "delete");

	return TRUE;
}


/**
 * midgard_object_delete:
 * @object: #MidgardObject instance
 * @check_dependents: dependents' check toggle
 *
 * Delete object's record(s) from database, but object's record is not fully deleted 
 * from database. Instead, it is marked as deleted , thus it is possible to undelete 
 * object later with midgard_schema_object_factory_object_undelete().
 *
 * If @check_dependents toggle is %TRUE, parameters and attachments storage will be queried,
 * if any of such exist and depend on deleted object.
 *
 * If given object's class has no metadata defined, object will be purged.
 *
 * Use midgard_object_purge() if you need to purge object's data from database.
 * 
 * Cases to return %FALSE:
 * <itemizedlist>
 * <listitem><para>
 * Object's has no storage defined ( MGD_ERR_OBJECT_NO_STORAGE )
 * </para></listitem>
 * <listitem><para>
 * Object's property guid is empty ( MGD_ERR_INVALID_PROPERTY_VALUE )
 * </para></listitem>
 * <listitem><para>
 * There are still dependent objects in database ( MGD_ERR_HAS_DEPENDENTS )
 * </para></listitem>
 * <listitem><para>
 * Unspecified internal error ( MGD_ERR_INTERNAL ) 
 * </para></listitem>
 * </itemizedlist> 
 *  
 * Returns: %TRUE if object is successfully marked as deleted, %FALSE otherwise.
 */ 
gboolean
midgard_object_delete (MidgardObject *object, gboolean check_dependents)
{
	g_return_val_if_fail (object != NULL, FALSE);

	if (!MGD_DBCLASS_METADATA_CLASS (MIDGARD_DBOBJECT_GET_CLASS (object))) 
		return midgard_object_purge (object, check_dependents);

	return _midgard_object_delete (object, check_dependents);
}

/**
 * midgard_object_purge:
 * @object: #MidgardObject instance
 * @check_dependents: dependents' check toggle
 *
 * Purge object's record(s) from database.
 *
 * If @check_dependents toggle is %TRUE, parameters and attachments storage will be queried,
 * if any of such exist and depend on deleted object.
 * 
 * Object's record(s) are purged from database without any possibility to recover.
 * After successfull call, only repligard table holds information about object's state.
 * Use midgard_object_delete(), if undelete facility is needed.
 * 
 * Cases to return %FALSE:
 * <itemizedlist>
 * <listitem><para>
 * Object has no storage defined ( MGD_ERR_OBJECT_NO_STORAGE )
 * </para></listitem>
 * <listitem><para>
 * Object's property guid value is empty ( MGD_ERR_INVALID_PROPERTY_VALUE ) 
 * </para></listitem>
 * <listitem><para>
 * There are still dependent objects in database ( MGD_ERR_HAS_DEPENDANTS )
 * </para></listitem>
 * <listitem><para>
 * No record has been deleted from database ( MGD_ERR_NOT_EXISTS )
 * </para></listitem>
 * <listitem><para>
 * Unspecified internal error ( MGD_ERR_INTERNAL ) 
 * </para></listitem>
 * </itemizedlist> 

 *
 * Returns: %TRUE if object has been succesfully purged from database, %FALSE otherwise.
 */ 
gboolean midgard_object_purge(MidgardObject *object, gboolean check_dependents)
{
	gint rv = 0;
	GString *dsql;
	const gchar *guid, *table;
	gchar *tmpstr;
	
	MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_OK);
	g_signal_emit(object, MIDGARD_OBJECT_GET_CLASS(object)->signal_action_purge, 0);	

	if (MIDGARD_DBOBJECT (object)->dbpriv->storage_data == NULL){
		g_warning("No schema attributes for class %s", 
				G_OBJECT_TYPE_NAME(object));
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_INTERNAL);
		return FALSE; 
	}
	
	MidgardObjectClass *klass = MIDGARD_OBJECT_GET_CLASS(object);
	table = midgard_core_class_get_table(MIDGARD_DBOBJECT_CLASS(klass));   
	if (table == NULL) {
		/* Object has no storage defined. Return FALSE as there is nothing to delete */
		g_warning("Object '%s' has no table or storage defined!", 
				G_OBJECT_TYPE_NAME(object));    
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_OBJECT_NO_STORAGE);    
		return FALSE;
	}

	guid = MGD_OBJECT_GUID(object);
	MidgardConnection *mgd = MGD_OBJECT_CNC (object);
		
	if (!guid){

		midgard_set_error(MGD_OBJECT_CNC(object),
				MGD_GENERIC_ERROR,
				MGD_ERR_INVALID_PROPERTY_VALUE, 
				"Guid property is NULL. ");
		return FALSE;
	}

	if (check_dependents && midgard_object_has_dependents(object)) {	
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC(object), MGD_ERR_HAS_DEPENDANTS);
		return FALSE;
	}

	dsql = g_string_new("DELETE ");
	g_string_append_printf(dsql, "FROM %s WHERE guid='%s' ", table, guid);
	
	guint size = midgard_quota_get_object_size(object);
	
	/* TODO , libgda, libgda */
	tmpstr = g_string_free(dsql, FALSE);
	rv = midgard_core_query_execute(MGD_OBJECT_CNC (object), tmpstr, FALSE);
	g_free(tmpstr);
	
	if (rv == 0) {
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_NOT_EXISTS);
		return FALSE;
	}
	
	midgard_quota_remove(object, size);
	
	if (MGD_CNC_REPLICATION (mgd)) {
		GValue tval = {0, };
		g_value_init(&tval, MIDGARD_TYPE_TIMESTAMP);
		midgard_timestamp_set_current_time(&tval);
		gchar *timedeleted = midgard_timestamp_get_string_from_value (&tval);	
		dsql = g_string_new("UPDATE repligard SET ");
		g_string_append_printf(dsql,
				"object_action = %d, object_action_date = '%s' "
				"WHERE typename='%s' AND guid='%s' ",	
				MGD_OBJECT_ACTION_PURGE, timedeleted,
				G_OBJECT_TYPE_NAME(object),
				MGD_OBJECT_GUID(object));

		rv = midgard_core_query_execute(MGD_OBJECT_CNC (object), dsql->str, FALSE);
		g_string_free(dsql, TRUE);
		g_free (timedeleted);	
		g_value_unset(&tval);
	}

	if (rv == 0) {

		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC(object), MGD_ERR_INTERNAL);
		return FALSE; 
	}

	g_signal_emit(object, MIDGARD_OBJECT_GET_CLASS(object)->signal_action_purged, 0);
	__dbus_send(object, "purge");

	return TRUE;  
}

/**
 * midgard_object_list:
 * @self: #MidgardObject instance
 * @n_objects: stores the length of returned array of objects
 *
 * Returned array stores object(s) which up property is equal to @self primary property
 * 
 * Returns: newly allocated, %NULL terminated array of #MidgardObject objects
 */
GObject **midgard_object_list(MidgardObject *self, guint *n_objects)
{
	MidgardObject *object = self;

	*n_objects = 0;

	g_assert(object != NULL);
	GParamSpec *fprop;
	const gchar *classname = G_OBJECT_TYPE_NAME (object);

	MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_OK);

	const gchar *primary_prop = 
		midgard_reflector_object_get_property_primary(classname);

	if (midgard_reflector_object_get_property_up(classname) == NULL) 
		return NULL;
	
	if ((fprop = g_object_class_find_property(
					G_OBJECT_GET_CLASS(G_OBJECT(object)),
					primary_prop)) != NULL){
		
		if (g_object_class_find_property(
						G_OBJECT_GET_CLASS(G_OBJECT(object)),
						midgard_reflector_object_get_property_up(classname)) == NULL ) {
			MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_NOT_EXISTS);
			return NULL;
		}

		MidgardQueryBuilder *builder =
			midgard_query_builder_new(MGD_OBJECT_CNC (object), 
					G_OBJECT_TYPE_NAME(object));

		if (!builder) {
			MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_INTERNAL);
			return NULL;
		}

		GValue pval = {0,};
		g_value_init(&pval,fprop->value_type);
		g_object_get_property(G_OBJECT(object), primary_prop, &pval);
		
		midgard_query_builder_add_constraint(builder,
				midgard_reflector_object_get_property_up(classname), "=", &pval);
		
		g_value_unset(&pval);
		GObject **objects = midgard_query_builder_execute(builder, n_objects);
		g_object_unref(builder);
				
		return objects;
	}
	return NULL;
}

/**
 * midgard_object_list_children:
 * @object: #MidgardObject instance
 * @childname: child typename
 * @n_objects: stores the length of returned array of objects
 *
 * Returns: newly allocated, %NULL terminated children objects array 
 */
GObject **midgard_object_list_children(MidgardObject *object, 
		const gchar *childcname, guint *n_objects)
{
	*n_objects = 0;

	GParamSpec *fprop ;
	const gchar *primary_prop = MIDGARD_DBOBJECT (object)->dbpriv->storage_data->primary;

	MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_OK);

	if ((childcname == NULL) || (MIDGARD_DBOBJECT (object)->dbpriv->storage_data->children == NULL)) {
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_NOT_EXISTS);    
		return NULL;
	}

	GSList *list;
	GSList *children = MIDGARD_DBOBJECT (object)->dbpriv->storage_data->children;
	gboolean found = FALSE;

	for (list = children; list != NULL; list = list->next) {

		if (list->data && g_str_equal(list->data, childcname))
			found = TRUE;
	}

	if (!found) {

		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_NOT_EXISTS);
		g_log(G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG,
				"Child type (%s) is not a child type of (%s)", 
				childcname, G_OBJECT_TYPE_NAME(object));
		return NULL;
	}
	
	MidgardObject *child = midgard_object_new(MGD_OBJECT_CNC (object), childcname, NULL);	

	if (midgard_reflector_object_get_property_parent(childcname) == NULL) {
		g_object_unref(child);
		MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_NOT_EXISTS);
		return NULL;
	}
	
	if ((fprop = g_object_class_find_property(
					G_OBJECT_GET_CLASS(object),
					primary_prop)) != NULL){
		
		MidgardQueryBuilder *builder =
			midgard_query_builder_new(MGD_OBJECT_CNC (object), 
					G_OBJECT_TYPE_NAME(child));
		
		if (!builder) {
			MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_INTERNAL);
			return NULL;
		}
		
		GValue pval = {0,};
		g_value_init(&pval,fprop->value_type);
		g_object_get_property(G_OBJECT(object), primary_prop, &pval);
		
		if (midgard_query_builder_add_constraint(builder, 
					midgard_reflector_object_get_property_parent(childcname)
					, "=", &pval)) {

			g_value_unset(&pval);
			GObject **objects = midgard_query_builder_execute(builder, n_objects);
			g_object_unref(builder);
						
			return objects;
			
		} else {
			
			MIDGARD_ERRNO_SET(MGD_OBJECT_CNC (object), MGD_ERR_INTERNAL);
			g_value_unset(&pval);         
			return NULL;                        
		}                            
	}
	return NULL;
}

/**
 * midgard_object_has_dependents:
 * @self: #MidgardObject instance
 *
 * Checks whether object has dependent objects. 
 *
 * Check is done with such particular order:
 *
 * <itemizedlist>
 * <listitem><para>
 * Objects of the same type 
 * </para></listitem>
 * <listitem><para>
 * Children objects
 * </para></listitem>
 * <listitem><para>
 * Parameters
 * </para></listitem>
 * <listitem><para>
 * Attachments
 * </para></listitem>
* </itemizedlist> 
 */
gboolean midgard_object_has_dependents(MidgardObject *self)
{
	g_return_val_if_fail(self != NULL, FALSE);

	if (midgard_core_object_has_dependents(self, G_OBJECT_TYPE_NAME(self)))
		return TRUE;

	GSList *list = NULL;
	GSList *children = MIDGARD_DBOBJECT (self)->dbpriv->storage_data->children;

	for (list = children ; list != NULL; list = list->next) {

		if (!list->data)
			continue;

		if (midgard_core_object_has_dependents(self, (const gchar *)list->data))
			return TRUE;
	}

	if (midgard_object_has_parameters(self))
		return TRUE;

	if (midgard_object_has_attachments(self))
		return TRUE;

	return FALSE;
}

/** 
 * midgard_object_get_by_path:
 * @self: #MidgardObject instance
 * @path: path at which object should be found
 *
 * Fetch object's record by path
 *
 * This function is slower than midgard_object_class_get_object_by_path, 
 * as it has to create new object instance and copy all properties.
 * 
 * #MidgardError is set by midgard_object_class_get_object_by_path in this case.
 * Read about midgard_object_class_get_object_by_path() for more details.
 * 
 * Returns: %TRUE if object is found, %FALSE otherwise.
 */ 
gboolean midgard_object_get_by_path(MidgardObject *self, const gchar *path)
{
	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	MidgardObject *object = 
		midgard_schema_object_factory_get_object_by_path(
				MGD_OBJECT_CNC (self), 
				G_OBJECT_TYPE_NAME(self),
				path);

	if (!object)
		return FALSE;

	_object_copy_properties(G_OBJECT(object), G_OBJECT(self));

	g_object_unref(object);

	return TRUE;
}

/** 
 * midgard_object_set_connection:
 * @self: #MidgardObject instance
 * @mgd: #MidgardConnection instance
 *
 * Set object's connection.
 *
 * This method associate object with connection being already initialized.
 * It's not required to use it in every aplication, however it's mandatory
 * when object has been initialized by some underlying library. For example, 
 * object pointer is available from g_object_new instead of midgard_object_new.
 * 
 * Already associated connection is silently ignored.
 */ 
void midgard_object_set_connection(MidgardObject *self, MidgardConnection *mgd)
{
	g_assert(self != NULL);
	g_assert(mgd != NULL);
	
	if (MGD_OBJECT_CNC (self) != NULL)
		return;
	
	MGD_OBJECT_CNC (self) = mgd;
}

/**
 * midgard_object_get_connection:
 * @self: #MidgardObject instance
 *
 * Returned #MidgardConnection is owned by core, and should not be freed.
 *
 * Returns: pointer to #MidgardConnection associated with given object.
 */ 
const MidgardConnection *midgard_object_get_connection(MidgardObject *self)
{
	g_assert(self != NULL);

	return (const MidgardConnection *) MIDGARD_OBJECT_GET_CLASS(MIDGARD_OBJECT(self))->get_connection(MIDGARD_DBOBJECT(self));
}

/**
 * midgard_object_approve:
 * @self: #MidgardObject instance
 *
 * Approve object.
 *
 * Approval functionality is not used by midgard core itself. 
 * Instead, it supports higher level's applications. It means, 
 * that there are no core methods ( like update or delete ) which 
 * do real check if object is approved. You should create own approval
 * workflow and logic with methods: midgard_object_is_approved(), 
 * midgard_object_unapprove() and this one.
 *
 * This method updates metadata properties: 
 * revisor, revised, revision, approver and approved.
 *
 * Cases to return %FALSE:
 * <itemizedlist>
 * <listitem><para>
 * No user logged in ( MGD_ERR_ACCESS_DENIED ) 
 * </para></listitem>
 * <listitem><para>
 * No active person ( MGD_ERR_INTERNAL ) 
 * </para></listitem>
 * <listitem><para>
 * Object is already approved
 * </para></listitem>
 * </itemizedlist> 

 * Returns: %TRUE if object has been approved, FALSE otherwise
 */ 
gboolean midgard_object_approve(MidgardObject *self)
{
	g_assert(self != NULL);
	g_object_ref(self);

	g_signal_emit(self, MIDGARD_OBJECT_GET_CLASS(self)->signal_action_approve, 0);

	MidgardConnection *mgd = MGD_OBJECT_CNC(self);
	MIDGARD_ERRNO_SET(mgd, MGD_ERR_OK);

	MidgardMetadata *metadata = MGD_DBOBJECT_METADATA (self);

	MidgardUser *user = midgard_connection_get_user(mgd);

	if (!user) {
		
		MIDGARD_ERRNO_SET(mgd, MGD_ERR_ACCESS_DENIED);
		g_object_unref(self);
		return FALSE;
	}

	const MidgardObject *person = midgard_user_get_person(user);

	if (!person) {
		
		MIDGARD_ERRNO_SET(mgd, MGD_ERR_INTERNAL);
		g_object_unref(self);
		return FALSE;
	}

	if (midgard_object_is_approved(self)) {

		g_warning("Object is already approved");
		return FALSE;
	}

	/* approved time value */
	GValue tval = {0, };
	midgard_timestamp_new_current (&tval);

	/* approver guid value */
	GValue gval = {0, };
	g_value_init(&gval, G_TYPE_STRING);
	g_value_set_string(&gval, MGD_OBJECT_GUID(person));

	/* approved toggle */
	GValue aval = {0, };
	g_value_init(&aval, G_TYPE_BOOLEAN);
	g_value_set_boolean(&aval, TRUE);

	/* increment revision */
	GValue rval = {0, };
	g_value_init(&rval, G_TYPE_UINT);
	g_value_set_uint(&rval, metadata->priv->revision+1);

	/* Transform timestamp to GdaTimestamp */
	GValue stval = {0, };
	g_value_init (&stval, GDA_TYPE_TIMESTAMP);
	g_value_transform (&tval, &stval);

	gboolean rv = midgard_core_query_update_object_fields(
			MIDGARD_DBOBJECT(self), 
			"metadata_approved", &stval,
			"metadata_approver", &gval, 
			"metadata_isapproved", &aval, 
			"metadata_revisor", &gval,
			"metadata_revised", &stval,
			"metadata_revision", &rval,
			NULL);

	/* Set object properties if query succeeded */
	if (rv) {
		
		metadata->priv->approve_is_set = TRUE;
		metadata->priv->is_approved = TRUE;
		
		midgard_core_metadata_set_approved (metadata, &tval);
		midgard_core_metadata_set_approver (metadata, &gval);
		midgard_core_metadata_set_revisor (metadata, &gval);
		midgard_core_metadata_set_revised (metadata, &tval);
		midgard_core_metadata_set_revision (metadata, &rval);
	}

	g_value_unset (&tval);
	g_value_unset (&stval);
	g_value_unset (&gval);
	g_value_unset (&aval);
	g_value_unset (&rval);

	g_object_unref(self);
	g_signal_emit(self, MIDGARD_OBJECT_GET_CLASS(self)->signal_action_approved, 0);

	return rv;
}

/**
 * midgard_object_is_approved:
 * @self: #MidgardObject instance
 *
 * Check whether object is approved.
 *
 * Returns: %TRUE if object is approved, %FALSE otherwise
 */ 
gboolean midgard_object_is_approved(MidgardObject *self)
{
	g_assert(self != NULL);

	g_object_ref(self);
	
	MidgardConnection *mgd = MGD_OBJECT_CNC(self);
	MIDGARD_ERRNO_SET(mgd, MGD_ERR_OK);

	MidgardMetadata *metadata = MGD_DBOBJECT_METADATA (self);

	if (metadata->priv->approve_is_set)
		return metadata->priv->is_approved;

	GString *where = g_string_new("");
	g_string_append_printf(where, "guid = '%s' ", MGD_OBJECT_GUID(self));

	GValue *aval = 
		midgard_core_query_get_field_value(mgd, 
				"metadata_isapproved", 
				midgard_core_class_get_table(MIDGARD_DBOBJECT_GET_CLASS(self)), 
				where->str);
	
	g_string_free(where, TRUE);

	if (!aval) {

		g_object_unref(self);
		return FALSE;
	}

	gboolean av = FALSE;
	MIDGARD_GET_BOOLEAN_FROM_VALUE(av, aval)

	metadata->priv->approve_is_set = TRUE;
	metadata->priv->is_approved = av;

	g_value_unset(aval);
	g_free(aval);

	g_object_unref(self);

	return av;
}

/**
 * midgard_object_unapprove:
 * @self: #MidgardObject instance
 *
 * Simply unapprove object. It doesn't affect any core functionality, 
 * like midgard_object_approve().
 *
 * This method updates metadata properties:
 * revisor, revised, revision, approver and approved
 *
 * Cases to return %FALSE:
 * <itemizedlist>
 * <listitem><para>
 * No user logged in ( MGD_ERR_ACCESS_DENIED ) 
 * </para></listitem>
 * <listitem><para>
 * Object is not approved
 * </para></listitem>
 * </itemizedlist> 
 *
 * Returns: %TRUE on success, %FALSE otherwise
 */ 
gboolean midgard_object_unapprove(MidgardObject *self)
{
	g_assert(self != NULL);

	g_object_ref(self);
	g_signal_emit(self, MIDGARD_OBJECT_GET_CLASS(self)->signal_action_unapprove, 0);

	MidgardConnection *mgd = MGD_OBJECT_CNC(self);
	MIDGARD_ERRNO_SET(mgd, MGD_ERR_OK);

	MidgardMetadata *metadata = MGD_DBOBJECT_METADATA (self);

	MidgardUser *user = midgard_connection_get_user(mgd);

	if (!user) {
		
		MIDGARD_ERRNO_SET(mgd, MGD_ERR_ACCESS_DENIED);
		g_object_unref(self);
		return FALSE;
	}

	const MidgardObject *person = midgard_user_get_person(user);

	if (!person) {
		
		MIDGARD_ERRNO_SET(mgd, MGD_ERR_INTERNAL);
		g_object_unref(self);
		return FALSE;
	}						         

	/* approved time value */
	GValue tval = {0, };
	midgard_timestamp_new_current (&tval);

	/* approver guid value */
	GValue gval = {0, };
	g_value_init(&gval, G_TYPE_STRING);
	g_value_set_string(&gval, MGD_OBJECT_GUID(person));

	/* approved toggle */
	GValue aval = {0, };
	g_value_init(&aval, G_TYPE_BOOLEAN);
	g_value_set_boolean(&aval, FALSE);

	/* increment revision */
	GValue rval = {0, };
	g_value_init(&rval, G_TYPE_UINT);
	g_value_set_uint(&rval, metadata->priv->revision+1);

	/* Transform timestamp to GdaTimestamp */
	GValue stval = {0, };
	g_value_init (&stval, GDA_TYPE_TIMESTAMP);
	g_value_transform (&tval, &stval);

	if (!midgard_object_is_approved(self)) {

		g_warning("Object is not approved");
		g_object_unref(self);
		return FALSE;
	}

	/* FIXME & TODO , I think we need revisor, revised and revision updates too */
	gboolean rv = midgard_core_query_update_object_fields(
			MIDGARD_DBOBJECT(self), 
			"metadata_approved", &stval,
			"metadata_approver", &gval, 
			"metadata_isapproved", &aval, 
			"metadata_revisor", &gval, 
			"metadata_revised", &stval, 
			"metadata_revision", &rval, 
			NULL);

	/* Set object properties if query succeeded */
	if (rv) {
		
		metadata->priv->approve_is_set = TRUE;
		metadata->priv->is_approved = FALSE;

		midgard_core_metadata_set_approved (metadata, &tval);
		midgard_core_metadata_set_approver (metadata, &gval);
		midgard_core_metadata_set_revisor (metadata, &gval);
		midgard_core_metadata_set_revised (metadata, &tval);
		midgard_core_metadata_set_revision (metadata, &rval);
	}

	g_value_unset (&tval);
	g_value_unset (&stval);
	g_value_unset (&gval);
	g_value_unset (&aval);
	g_value_unset (&rval);

	g_signal_emit(self, MIDGARD_OBJECT_GET_CLASS(self)->signal_action_unapproved, 0);
	g_object_unref(self);

	return rv;
}

/**
 * midgard_object_lock:
 * @self: #MidgardObject instance
 *
 * Lock object.
 *
 * This method doesn't affect any core's functionality like midgard_object_approve.
 * You should create own locking workflow and logic with methods:
 * midgard_object_is_locked(), midgard_object_unlock() and this one.
 *
 * Updates metadata properties:
 * locker, locked, revisor, revised and revision
 *
 * Cases to return %FALSE:
 * <itemizedlist>
 * <listitem><para>
 * No user logged in ( MGD_ERR_ACCESS_DENIED ) 
 * </para></listitem>
 * <listitem><para>
 * Metadata class not defined for given object's class ( MGD_ERR_NO_METADATA ) 
 * </para></listitem>
 * <listitem><para>
 * Object is already locked ( MGD_ERR_OBJECT_IS_LOCKED )
 * </para></listitem>
 * </itemizedlist> 
 *
 * Returns: %TRUE on success, %FALSE otherwise
 */ 
gboolean midgard_object_lock(MidgardObject *self)
{
	g_assert(self != NULL);

	g_object_ref(self);
	MidgardObjectClass *klass = MIDGARD_OBJECT_GET_CLASS (self);
	g_signal_emit(self, klass->signal_action_lock, 0);

	MidgardConnection *mgd = MGD_OBJECT_CNC(self);
	MIDGARD_ERRNO_SET(mgd, MGD_ERR_OK);

	MidgardMetadata *metadata = MGD_DBOBJECT_METADATA (self);

	if (!MGD_DBCLASS_METADATA_CLASS (MIDGARD_DBOBJECT_CLASS (klass))) {

		MIDGARD_ERRNO_SET (mgd, MGD_ERR_NO_METADATA);
		g_object_unref (self);
		return FALSE;
	}

	if (!MIDGARD_USER(mgd->priv->user)) {
		
		MIDGARD_ERRNO_SET(mgd, MGD_ERR_ACCESS_DENIED);
		g_object_unref(self);
		return FALSE;
	}

	if (midgard_object_is_locked(self)) {

		g_warning("Object is already locked");
		MIDGARD_ERRNO_SET(mgd, MGD_ERR_OBJECT_IS_LOCKED);
		g_object_unref(self);
		return FALSE;
	}

	/* approved time value */
	GValue tval = {0, };
	midgard_timestamp_new_current (&tval);

	/* approver guid value */
	GValue gval = {0, };
	g_value_init (&gval, G_TYPE_STRING);

	MidgardObject *person = MGD_CNC_PERSON (mgd);
	if (person)
		g_value_set_string (&gval, MGD_OBJECT_GUID(person));
	else
		g_value_set_string (&gval, "");

	/* approved toggle */
	GValue aval = {0, };
	g_value_init (&aval, G_TYPE_BOOLEAN);
	g_value_set_boolean (&aval, TRUE);

	/* increment revision */
	GValue rval = {0, };
	g_value_init (&rval, G_TYPE_UINT);
	g_value_set_uint (&rval, metadata->priv->revision+1);

	/* Transform timestamp to string */
	GValue stval = {0, };
	g_value_init (&stval, G_TYPE_STRING);
	g_value_transform (&tval, &stval);

	gboolean rv = midgard_core_query_update_object_fields(
			MIDGARD_DBOBJECT(self), 
			"metadata_locked", &stval,
			"metadata_locker", &gval, 
			"metadata_islocked", &aval, 
			"metadata_revisor", &gval, 
			"metadata_revised", &stval, 
			"metadata_revision", &rval,
			NULL);

	/* Set object properties if query succeeded */
	if (rv) {
		
		metadata->priv->lock_is_set = TRUE;
		metadata->priv->is_locked = TRUE;

		midgard_core_metadata_set_locked (metadata, &tval);
		midgard_core_metadata_set_locker (metadata, &gval);
		midgard_core_metadata_set_revisor (metadata, &gval);
		midgard_core_metadata_set_revised (metadata, &tval);
		midgard_core_metadata_increase_revision (metadata);
	}

	g_value_unset (&tval);
	g_value_unset (&stval);
	g_value_unset (&gval);
	g_value_unset (&aval);
	g_value_unset (&rval);

	g_signal_emit (self, MIDGARD_OBJECT_GET_CLASS(self)->signal_action_locked, 0);
	g_object_unref (self);

	return rv;
}

/**
 * midgard_object_is_locked:
 * @self: #MidgardObject instance
 *
 * Check whether object is locked
 * 
 * Cases to return %FALSE:
 * <itemizedlist>
 * <listitem><para>
 * Metadata class not defined for given object's class ( MGD_ERR_NO_METADATA ) 
 * </para></listitem>
 * </itemizedlist> 
 *
 * Returns: %TRUE on success, %FALSE otherwise
 */ 
gboolean midgard_object_is_locked(MidgardObject *self)
{
	g_assert(self != NULL);

	g_object_ref(self);
	
	MidgardConnection *mgd = MGD_OBJECT_CNC(self);
	MIDGARD_ERRNO_SET(mgd, MGD_ERR_OK);

	MidgardMetadata *metadata = MGD_DBOBJECT_METADATA (self);

	MidgardObjectClass *klass = MIDGARD_OBJECT_GET_CLASS (self);

	if (!MGD_DBCLASS_METADATA_CLASS (MIDGARD_DBOBJECT_CLASS (klass))) {

		MIDGARD_ERRNO_SET (mgd, MGD_ERR_NO_METADATA);
		g_object_unref (self);
		return FALSE;
	}

	if (metadata->priv->lock_is_set)
		return metadata->priv->is_locked;

	GString *where = g_string_new("");
	g_string_append_printf(where, "guid = '%s' ", MGD_OBJECT_GUID (self));

	GValue *aval = 
		midgard_core_query_get_field_value(MGD_OBJECT_CNC (self), 
				"metadata_islocked", 
				midgard_core_class_get_table(MIDGARD_DBOBJECT_GET_CLASS(self)), 
				where->str);
	
	g_string_free(where, TRUE);

	if (!aval) {

		g_object_unref(self);
		return FALSE;
	}

	gboolean av = FALSE;
	MIDGARD_GET_BOOLEAN_FROM_VALUE(av, aval)

	metadata->priv->lock_is_set = TRUE;
	metadata->priv->is_locked = av;

	g_value_unset(aval);
	g_free(aval);

	g_object_unref(self);

	return av;

}

/**
 * midgard_object_unlock:
 * @self: #MidgardObject instance
 *
 * Unlock object.
 *
 * Cases to return %FALSE:
 * <itemizedlist>
 * <listitem><para>
 * No user logged in ( MGD_ERR_ACCESS_DENIED ) 
 * </para></listitem>
 * <listitem><para>
 * Object is not locked ( FIXME )
 * </para></listitem>
 * <listitem><para>
 * Metadata class not defined for given object's class ( MGD_ERR_NO_METADATA ) 
 * </para></listitem>
 * </itemizedlist> 
 *
 * Returns: %TRUE on success, %FALSE otherwise
 */ 
gboolean midgard_object_unlock(MidgardObject *self)
{
	g_assert(self != NULL);

	g_object_ref(self);
	MidgardObjectClass *klass = MIDGARD_OBJECT_GET_CLASS (self);
	g_signal_emit(self, klass->signal_action_unlock, 0);

	MidgardMetadata *metadata = MGD_DBOBJECT_METADATA (self);

	MidgardConnection *mgd = MGD_OBJECT_CNC(self);
	MIDGARD_ERRNO_SET(mgd, MGD_ERR_OK);

	if (!MGD_DBCLASS_METADATA_CLASS (MIDGARD_DBOBJECT_CLASS (klass))) {

		MIDGARD_ERRNO_SET (mgd, MGD_ERR_NO_METADATA);
		g_object_unref (self);
		return FALSE;
	}

	if (!MIDGARD_USER(mgd->priv->user)) {
		
		MIDGARD_ERRNO_SET(mgd, MGD_ERR_ACCESS_DENIED);
		g_object_unref(self);
		return FALSE;
	}

	/* approved time value */
	GValue tval = {0, };
	midgard_timestamp_new_current (&tval);

	/* approver guid value */
	GValue gval = {0, };
	g_value_init(&gval, G_TYPE_STRING);

	MidgardObject *person = MGD_CNC_PERSON (mgd);
	if (person)
		g_value_set_string (&gval, MGD_OBJECT_GUID(person));
	else
		g_value_set_string (&gval, "");

	/* approved toggle */
	GValue aval = {0, };
	g_value_init(&aval, G_TYPE_BOOLEAN);
	g_value_set_boolean(&aval, FALSE);

	/* increment revision */
	GValue rval = {0, };
	g_value_init(&rval, G_TYPE_UINT);
	g_value_set_uint(&rval, metadata->priv->revision+1);

	/* Transform timestamp to GdaTimestamp */
	GValue stval = {0, };
	g_value_init (&stval, GDA_TYPE_TIMESTAMP);
	g_value_transform (&tval, &stval);

	if (!midgard_object_is_locked(self)) {

		g_warning("Object is not locked");
		g_object_unref(self);
		return FALSE;
	}

	gboolean rv = midgard_core_query_update_object_fields(
			MIDGARD_DBOBJECT(self), 
			"metadata_locker", &gval,
			"metadata_locked", &stval, 
			"metadata_islocked", &aval, 
			"metadata_revisor", &gval, 
			"metadata_revised", &stval, 
			"metadata_revision", &rval,
			NULL);

	/* Set object properties if query succeeded */
	if (rv) {
		
		metadata->priv->lock_is_set = TRUE;
		metadata->priv->is_locked = FALSE;

		midgard_core_metadata_set_locked (metadata, &tval);
		midgard_core_metadata_set_locker (metadata, &gval);
		midgard_core_metadata_set_revisor (metadata, &gval);
		midgard_core_metadata_set_revised (metadata, &tval);
		midgard_core_metadata_increase_revision (metadata);
	}

	g_value_unset (&tval);
	g_value_unset (&stval);
	g_value_unset (&gval);
	g_value_unset (&aval);
	g_value_unset (&rval);

	g_signal_emit (self, MIDGARD_OBJECT_GET_CLASS(self)->signal_action_unlocked, 0);
	g_object_unref (self);

	return rv;
}

/* C# helpers */
GValue *
midgard_object_get_schema_property (MidgardObject *self, const gchar *property)
{
	g_return_val_if_fail (self != NULL, NULL);
	g_return_val_if_fail (property != NULL, NULL);

	GParamSpec *pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (G_OBJECT (self)), property);
	if (!pspec) 
		return NULL;

	GValue *value = g_new0 (GValue, 1);
	g_value_init (value, pspec->value_type);
	g_object_get_property (G_OBJECT (self), property, value);

	return value;
}

void midgard_object_set_schema_property (MidgardObject *self, const gchar *property, GValue *value)
{
	g_return_if_fail (self != NULL);
	g_return_if_fail (property != NULL);

	g_object_set_property (G_OBJECT (self), property, value);
	return;
}

