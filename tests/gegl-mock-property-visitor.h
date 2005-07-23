#ifndef __GEGL_MOCK_PAD_VISITOR_H__
#define __GEGL_MOCK_PAD_VISITOR_H__

#include "gegl-visitor.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define GEGL_TYPE_MOCK_PAD_VISITOR               (gegl_mock_pad_visitor_get_type ())
#define GEGL_MOCK_PAD_VISITOR(obj)               (G_TYPE_CHECK_INSTANCE_CAST ((obj), GEGL_TYPE_MOCK_PAD_VISITOR, GeglMockPadVisitor))
#define GEGL_MOCK_PAD_VISITOR_CLASS(klass)       (G_TYPE_CHECK_CLASS_CAST ((klass),  GEGL_TYPE_MOCK_PAD_VISITOR, GeglMockPadVisitorClass))
#define GEGL_IS_MOCK_PAD_VISITOR(obj)            (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GEGL_TYPE_MOCK_PAD_VISITOR))
#define GEGL_IS_MOCK_PAD_VISITOR_CLASS(klass)    (G_TYPE_CHECK_CLASS_TYPE ((klass),  GEGL_TYPE_MOCK_PAD_VISITOR))
#define GEGL_MOCK_PAD_VISITOR_GET_CLASS(obj)     (G_TYPE_INSTANCE_GET_CLASS ((obj),  GEGL_TYPE_MOCK_PAD_VISITOR, GeglMockPadVisitorClass))

typedef struct _GeglMockPadVisitor GeglMockPadVisitor;
struct _GeglMockPadVisitor
{
       GeglVisitor visitor;
};

typedef struct _GeglMockPadVisitorClass GeglMockPadVisitorClass;
struct _GeglMockPadVisitorClass
{
       GeglVisitorClass visitor_class;
};

GType         gegl_mock_pad_visitor_get_type          (void) G_GNUC_CONST;

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
