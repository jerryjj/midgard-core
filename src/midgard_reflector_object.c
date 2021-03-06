/* 
 * Copyright (C) 2010 Piotr Pokora <piotrek.pokora@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "midgard_reflector_object.h"
#include "schema.h"
#include "midgard_object.h"
#include "midgard_core_object_class.h"
#include "midgard_core_object.h"

#define _GET_CLASS_BY_NAME(__name, __retval) \
	GObjectClass *klass = g_type_class_peek (g_type_from_name (__name)); \
	g_return_val_if_fail (MIDGARD_IS_DBOBJECT_CLASS (klass), __retval);

#define _GET_TYPE_ATTR(__klass) \
	 MgdSchemaTypeAttr *type_attr = \
		midgard_core_class_get_type_attr(MIDGARD_DBOBJECT_CLASS(__klass))

/**
 * midgard_reflector_object_get_property_primary:
 * @classname: Name of the class
 *
 * Returns: (transfer none): Name of property which is defined as primary for given class or %NULL.
 * Since: 10.05
 */  
const gchar* 
midgard_reflector_object_get_property_primary (const gchar *classname)
{
	g_return_val_if_fail (classname != NULL, NULL);

	_GET_CLASS_BY_NAME (classname, NULL);
	_GET_TYPE_ATTR (klass);

	return type_attr->primary;
}

/**
 * midgard_reflector_object_get_property_up:
 * @classname: Name of the class
 *
 * Returns: (transfer none): Name of property which is defined as 'up' for given class or %NULL.
 * Since: 10.05
 */  
const gchar*
midgard_reflector_object_get_property_up (const gchar *classname)
{
	g_return_val_if_fail (classname != NULL, NULL);

	_GET_CLASS_BY_NAME (classname, NULL);
	_GET_TYPE_ATTR (klass);

	return type_attr->property_up;
}

/**
 * midgard_reflector_object_get_property_unique:
 * @classname: Name of the class
 *
 * Returns: (transfer none): Name of property which is defined unique for given class, or %NULL.
 * Since: 10.05
 */  
const gchar*
midgard_reflector_object_get_property_unique (const gchar *classname)
{
	g_return_val_if_fail (classname != NULL, NULL);

	_GET_CLASS_BY_NAME (classname, NULL);
	_GET_TYPE_ATTR (klass);

	return type_attr->unique_name;
}

/**
 * midgard_reflector_object_get_property_parent:
 * @classname: Name of the class
 *
 * Returns: (transfer none): Name of property which is defined as 'parent' for given class or %NULL.
 * Since: 10.05
 */  
const gchar*
midgard_reflector_object_get_property_parent (const gchar *classname)
{
	g_return_val_if_fail (classname != NULL, NULL);

	_GET_CLASS_BY_NAME (classname, NULL);
	_GET_TYPE_ATTR (klass);

	return type_attr->property_parent;
}

/**
 * midgard_reflector_object_list_children:
 * @classname: Name of the class
 * @n_children: pointer to store number of children classes
 *
 * Returns newly allocated, children ( in midgard tree ) classes' names. 
 * Returned array should be freed if no longer needed without freeing array's elements. 
 *
 * Returns: (transfer container): array of strings or %NULL.
 * Since: 10.05
 */  
gchar**
midgard_reflector_object_list_children (const gchar *classname, guint *n_children)
{
	g_return_val_if_fail (classname != NULL, NULL);

	_GET_CLASS_BY_NAME (classname, NULL);
	_GET_TYPE_ATTR (klass);

	if (!type_attr->children)
		return NULL;

	 GSList *list;
	 GSList *children = type_attr->children;
	 guint i = 0;

	 gchar **children_class = g_new (gchar*, g_slist_length (children));

	 for (list = children; list != NULL; list = list->next, i++) 
		 children_class[i] = (gchar *) list->data;

	 *n_children = i;
	 
	 return children_class;
}

/**
 * midgard_reflector_object_has_metadata:
 * @classname: Name of the class
 *
 * Checks whether metadata is defined for given @classname.
 *
 * Returns: %TRUE or %FALSE
 * Since: 10.05
 */  
gboolean
midgard_reflector_object_has_metadata_class (const gchar *classname)
{
	g_return_val_if_fail (classname != NULL, FALSE);

	_GET_CLASS_BY_NAME (classname, FALSE);
	_GET_TYPE_ATTR (klass);

	if (type_attr->metadata_class_name)
		return TRUE;

	return FALSE;
}

/**
 * midgard_reflector_object_get_metadata_class:
 * @classname: Name of the class
 *
 * Returns: (transfer none): Name of the metadata class of the given one or %NULL.
 * Since: 10.05
 */  
const gchar*
midgard_reflector_object_get_metadata_class (const gchar *classname)
{
	g_return_val_if_fail (classname != NULL, NULL);

	_GET_CLASS_BY_NAME (classname, NULL);
	_GET_TYPE_ATTR (klass);

	return (const gchar *)type_attr->metadata_class_name;
}

/**
 * midgard_reflector_object_get_schema_value:
 * @classname: Name of the class
 * @name: node's name declared for given @klass
 *
 *
 * Returns: (transfer none): value of given node's name or %NULL.
 * Since: 10.05
 */  
const gchar*
midgard_reflector_object_get_schema_value (const gchar *classname, const gchar *name)
{
	g_return_val_if_fail (classname != NULL, NULL);

	_GET_CLASS_BY_NAME (classname, NULL);
	_GET_TYPE_ATTR (klass);

	return (const gchar *) g_hash_table_lookup (type_attr->user_values, (gpointer) name);
}

/* GOBJECT ROUTINES */

GType 
midgard_reflector_object_get_type (void)
{
	static GType type = 0;
	if (type == 0) {
		static const GTypeInfo info = {
			sizeof (MidgardReflectorObjectClass),
			NULL,           /* base_init */
			NULL,           /* base_finalize */
			NULL,
			NULL,           /* class_finalize */
			NULL,           /* class_data */
			sizeof (MidgardReflectorObject),
			0,              /* n_preallocs */
			NULL /* instance_init */
		};
		type = g_type_register_static (G_TYPE_OBJECT, "MidgardReflectorObject", &info, G_TYPE_FLAG_ABSTRACT);
	}
	return type;
}
