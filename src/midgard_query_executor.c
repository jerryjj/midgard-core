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

#include "midgard_query_executor.h"
#include "midgard_core_query.h"

/* MidgardQueryExecutor properties */
enum {
	PROPERTY_RESULTS_COUNT = 1
};

MidgardQueryExecutor *
midgard_query_executor_new (MidgardConnection *mgd, MidgardQueryStorage *storage)
{
	return NULL;
}

gboolean
midgard_query_executor_set_constraint (MidgardQueryExecutor *self, MidgardQueryConstraintSimple *constraint)
{
	return MIDGARD_QUERY_EXECUTOR_GET_CLASS (self)->set_constraint (self, constraint);
}

gboolean
midgard_query_executor_set_limit (MidgardQueryExecutor *self, guint limit)
{
	return MIDGARD_QUERY_EXECUTOR_GET_CLASS (self)->set_limit (self, limit);
}

gboolean midgard_query_executor_set_offset (MidgardQueryExecutor *self, guint offset)
{
	return MIDGARD_QUERY_EXECUTOR_GET_CLASS (self)->set_offset (self, offset);
}

gboolean
midgard_query_executor_add_order (MidgardQueryExecutor *self, MidgardQueryProperty *property, const gchar *type)
{
	return MIDGARD_QUERY_EXECUTOR_GET_CLASS (self)->add_order (self, property, type);
}

gboolean
midgard_query_executor_add_join (MidgardQueryExecutor *self, const gchar *join_type, 
		MidgardQueryProperty *left_property, MidgardQueryProperty *right_property)
{
	return MIDGARD_QUERY_EXECUTOR_GET_CLASS (self)->add_join (self, join_type, left_property, right_property);
}

guint 
midgard_query_executor_get_results_count (MidgardQueryExecutor *self)
{
	return MIDGARD_QUERY_EXECUTOR_GET_CLASS (self)->get_results_count (self);
}

gboolean
midgard_query_executor_execute (MidgardQueryExecutor *self)
{
	return MIDGARD_QUERY_EXECUTOR_GET_CLASS (self)->execute (self);
}

/* GOBJECT ROUTINES */

static GObjectClass *parent_class= NULL;

static GObject *
_midgard_query_executor_constructor (GType type,
		guint n_construct_properties,
		GObjectConstructParam *construct_properties)
{
	GObject *object = (GObject *)
		G_OBJECT_CLASS (parent_class)->constructor (type,
				n_construct_properties,
				construct_properties);

	MIDGARD_QUERY_EXECUTOR (object)->priv = g_new (MidgardQueryExecutorPrivate, 1);
	MIDGARD_QUERY_EXECUTOR (object)->priv->mgd = NULL;
	MIDGARD_QUERY_EXECUTOR (object)->priv->storage = NULL;
	MIDGARD_QUERY_EXECUTOR (object)->priv->constraint = NULL;
	MIDGARD_QUERY_EXECUTOR (object)->priv->orders = NULL;
	MIDGARD_QUERY_EXECUTOR (object)->priv->joins = NULL;
	MIDGARD_QUERY_EXECUTOR (object)->priv->limit = -1;
	MIDGARD_QUERY_EXECUTOR (object)->priv->offset = -1;
	MIDGARD_QUERY_EXECUTOR (object)->priv->resultset = NULL;
	MIDGARD_QUERY_EXECUTOR (object)->priv->tableid = 0;
	MIDGARD_QUERY_EXECUTOR (object)->priv->table_alias = NULL;
	MIDGARD_QUERY_EXECUTOR (object)->priv->stmt = NULL;
	MIDGARD_QUERY_EXECUTOR (object)->priv->joinid = 0;
	MIDGARD_QUERY_EXECUTOR (object)->priv->results_count = 0;
	MIDGARD_QUERY_EXECUTOR (object)->priv->read_only = TRUE;

	return G_OBJECT(object);
}

static void
_midgard_query_executor_dispose (GObject *object)
{	
	parent_class->dispose (object);
}

static void 
_midgard_query_executor_finalize (GObject *object)
{
	MidgardQueryExecutor *self = MIDGARD_QUERY_EXECUTOR (object);

	if (self->priv->joins) {
		g_slist_foreach (self->priv->joins, (GFunc) g_free, NULL);
		g_slist_free (self->priv->joins);
		self->priv->joins = NULL;
	}

	if (self->priv->orders) {
		g_slist_foreach (self->priv->orders, (GFunc) g_free, NULL);
		g_slist_free (self->priv->orders);
		self->priv->orders = NULL;
	}

	g_free (self->priv->table_alias);
	self->priv->table_alias = NULL;

	g_free (self->priv);
	self->priv = NULL;

	parent_class->finalize (object);
}

static void
_midgard_query_executor_get_property (GObject *object, guint property_id,
		GValue *value, GParamSpec *pspec)
{
	MidgardQueryExecutor *self = MIDGARD_QUERY_EXECUTOR (object);

	switch (property_id) {
		
		case PROPERTY_RESULTS_COUNT:
			g_value_set_uint (value, self->priv->results_count);
			break;

		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(self, property_id, pspec);
			break;
	}
}

static void 
_midgard_query_executor_class_init (MidgardQueryExecutorClass *klass, gpointer class_data)
{
       	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	parent_class = g_type_class_peek_parent (klass);

	object_class->constructor = _midgard_query_executor_constructor;
	object_class->dispose = _midgard_query_executor_dispose;
	object_class->finalize = _midgard_query_executor_finalize;
	object_class->get_property = _midgard_query_executor_get_property;

	GParamSpec *pspec = g_param_spec_uint ("resultscount",
			"Number of objects.", "",
			0, G_MAXUINT32, 0, G_PARAM_READABLE);
	g_object_class_install_property (object_class,
			PROPERTY_RESULTS_COUNT, pspec);

	klass->set_constraint = NULL;
	klass->set_limit = NULL;
	klass->set_offset = NULL;
	klass->add_order = NULL;
	klass->add_join = NULL;
	klass->execute = NULL;
	klass->get_results_count = NULL;
}

GType
midgard_query_executor_get_type (void)
{
   	static GType type = 0;
	if (type == 0) {
		static const GTypeInfo info = {
			sizeof (MidgardQueryExecutorClass),
			NULL,           /* base_init */
			NULL,           /* base_finalize */
			(GClassInitFunc) _midgard_query_executor_class_init,
			NULL,           /* class_finalize */
			NULL,           /* class_data */
			sizeof (MidgardQueryExecutor),
			0,              /* n_preallocs */
			NULL /* instance_init */
		};
		type = g_type_register_static (G_TYPE_OBJECT, "MidgardQueryExecutor", &info, G_TYPE_FLAG_ABSTRACT);
	}
	return type;
}
