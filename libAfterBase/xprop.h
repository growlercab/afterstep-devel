#ifndef XPROP_H_HEADER_INCLUDED
#define XPROP_H_HEADER_INCLUDED

#include <X11/Xmd.h>

/************************************************************************/
/*                Utility data structures :				*/
/************************************************************************/

typedef struct AtomXref
{
  char *name;
  Atom *variable;
  ASFlagType flag;
  Atom atom;
}
AtomXref;

/*************************************************************************/
/*                           Interface                                   */
/*************************************************************************/
/* X Atoms : */
Bool intern_atom_list (AtomXref * list);
void translate_atom_list (ASFlagType *trg, AtomXref * xref, Atom * list,
                          long nitems);
void print_list_hints( stream_func func, void* stream, ASFlagType flags,
                       AtomXref *xref, const char *prompt );
Bool read_32bit_proplist (Window w, Atom property, long estimate,
                          CARD32 ** list, long *nitems);
Bool read_text_property (Window w, Atom property, XTextProperty ** trg);
Bool read_32bit_property (Window w, Atom property, CARD32 * trg);

char *text_property2string( XTextProperty *tprop);

void print_text_property( stream_func func, void* stream, XTextProperty *tprop, const char *prompt );
void free_text_property (XTextProperty ** trg);

#endif /* XPROP_H_HEADER_INCLUDED */
