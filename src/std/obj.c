#include "hl.h"
#include <string.h>

static void hl_lookup_insert( hl_field_lookup *l, int size, int hash, hl_type *t, int index ) {
	int min = 0;
	int max = size;
	int pos;
	while( min < max ) {
		int mid = (min + max) >> 1;
		int h = l[mid].hashed_name;
		if( h < hash ) min = mid + 1; else max = mid;
	}
	pos = (min + max) >> 1;
	memcpy(l + pos + 1, l + pos, (size - pos) * sizeof(hl_field_lookup));
	l[pos].field_index = index;
	l[pos].hashed_name = hash;
	l[pos].t = t;
}

hl_field_lookup *hl_lookup_find( hl_field_lookup *l, int size, int hash ) {
	int min = 0;
	int max = size;
	while( min < max ) {
		int mid = (min + max) >> 1;
		int h = l[mid].hashed_name;
		if( h < hash ) min = mid + 1; else if( h > hash ) max = mid; else return l + mid;
	}
	return NULL;
}

static int hl_lookup_find_index( hl_field_lookup *l, int size, int hash ) {
	int min = 0;
	int max = size;
	while( min < max ) {
		int mid = (min + max) >> 1;
		int h = l[mid].hashed_name;
		if( h < hash ) min = mid + 1; else if( h > hash ) max = mid; else return mid;
	}
	return (min + max) >> 1;
}

static hl_field_lookup *obj_resolve_field( hl_type_obj *o, int hfield ) {
	hl_runtime_obj *rt = o->rt;
	do {
		hl_field_lookup *f = hl_lookup_find(rt->lookup,rt->nlookup,hfield);
		if( f ) return f;
		rt = rt->parent;
	} while( rt );
	return NULL;
}

static int hl_cache_count = 0;
static int hl_cache_size = 0;
static hl_field_lookup *hl_cache = NULL;

int hl_hash( vbyte *b ) {
	return hl_hash_gen((uchar*)b,true);
}

int hl_hash_gen( const uchar *name, bool cache_name ) {
	int h = 0;
	const uchar *oname = name;
	while( *name ) {
		h = 223 * h + (unsigned)*name;
		name++;
	}
	h %= 0x1FFFFF7B;
	if( cache_name ) {
		hl_field_lookup *l = hl_lookup_find(hl_cache, hl_cache_count, h);
		if( l == NULL ) {
			if( hl_cache_size == hl_cache_count ) {
				// resize
				int newsize = hl_cache_size ? (hl_cache_size * 3) >> 1 : 16;
				hl_field_lookup *cache = (hl_field_lookup*)malloc(sizeof(hl_field_lookup) * newsize);
				memcpy(cache,hl_cache,sizeof(hl_field_lookup) * hl_cache_count);
				free(hl_cache);
				hl_cache = cache;
				hl_cache_size = newsize;
			}
			hl_lookup_insert(hl_cache,hl_cache_count++,h,(hl_type*)ustrdup(oname),0);
		}
	}
	return h;
}

const uchar *hl_field_name( int hash ) {
	hl_field_lookup *l = hl_lookup_find(hl_cache, hl_cache_count, hash);
	return l ? (uchar*)l->t : USTR("???");
}

void hl_cache_free() {
	int i;
	for(i=0;i<hl_cache_count;i++)
		free(hl_cache[i].t);
	free(hl_cache);
	hl_cache = NULL;
	hl_cache_count = hl_cache_size = 0;
}

/**
	Builds class metadata (fields indexes, etc.)
	Does not require the method table to be finalized.
**/
hl_runtime_obj *hl_get_obj_rt( hl_type *ot ) {
	hl_type_obj *o = ot->obj;
	hl_module_context *m = o->m;
	hl_alloc *alloc = &m->alloc;
	hl_runtime_obj *p = NULL, *t;
	int i, size, start;
	if( o->rt ) return o->rt;
	if( o->super ) p = hl_get_obj_rt(o->super);
	t = (hl_runtime_obj*)hl_malloc(alloc,sizeof(hl_runtime_obj));
	t->t = ot;
	t->nfields = o->nfields + (p ? p->nfields : 0);
	t->nproto = p ? p->nproto : 0;
	t->nlookup = o->nfields + o->nproto;
	t->lookup = (hl_field_lookup*)hl_malloc(alloc,sizeof(hl_field_lookup) * t->nlookup);
	t->fields_indexes = (int*)hl_malloc(alloc,sizeof(int)*t->nfields);
	t->toStringFun = NULL;
	t->compareFun = NULL;
	t->castFun = NULL;
	t->getFieldFun = NULL;
	t->parent = p;

	// fields indexes
	start = 0;
	if( p ) {
		start = p->nfields;
		memcpy(t->fields_indexes, p->fields_indexes, sizeof(int)*p->nfields);
	}
	size = p ? p->size : HL_WSIZE; // hl_type*
	for(i=0;i<o->nfields;i++) {
		hl_type *ft = o->fields[i].t;
		size += hl_pad_size(size,ft);
		t->fields_indexes[i+start] = size;
		hl_lookup_insert(t->lookup,i,o->fields[i].hashed_name,o->fields[i].t,size);
		size += hl_type_size(ft);
	}
	t->size = size;
	t->methods = NULL;
	o->rt = t;
	ot->vobj_proto = NULL;

	// fields lookup
	size = 0;
	for(i=0;i<o->nproto;i++) {
		hl_obj_proto *p = o->proto + i;
		if( p->pindex >= t->nproto ) t->nproto = p->pindex + 1;
		hl_lookup_insert(t->lookup,i + o->nfields,p->hashed_name,m->functions_types[p->findex],-(i+1));
	}
	return t;
}

/**
	Fill class prototype with method pointers.
	Requires method table to be finalized
**/
hl_runtime_obj *hl_get_obj_proto( hl_type *ot ) {
	hl_type_obj *o = ot->obj;
	hl_module_context *m = o->m;
	hl_alloc *alloc = &m->alloc;
	hl_runtime_obj *p = NULL, *t = hl_get_obj_rt(ot);
	hl_field_lookup *strField, *cmpField, *castField, *getField;
	int i;
	if( ot->vobj_proto ) return t;
	if( o->super ) p = hl_get_obj_proto(o->super);

	strField = hl_lookup_find(t->lookup,t->nlookup,hl_hash_gen(USTR("__string"),false));
	cmpField = hl_lookup_find(t->lookup,t->nlookup,hl_hash_gen(USTR("__compare"),false));
	castField = hl_lookup_find(t->lookup,t->nlookup,hl_hash_gen(USTR("__cast"),false));
	getField = hl_lookup_find(t->lookup,t->nlookup,hl_hash_gen(USTR("__get_field"),false));
	t->toStringFun = strField ? m->functions_ptrs[o->proto[-(strField->field_index+1)].findex] : (p ? p->toStringFun : NULL);	
	t->compareFun = cmpField ? m->functions_ptrs[o->proto[-(cmpField->field_index+1)].findex] : (p ? p->compareFun : NULL);	
	t->castFun = castField ? m->functions_ptrs[o->proto[-(castField->field_index+1)].findex] : (p ? p->castFun : NULL);	
	t->getFieldFun = getField ? m->functions_ptrs[o->proto[-(getField->field_index+1)].findex] : (p ? p->getFieldFun : NULL);

	if( t->nproto ) {
		void **fptr = (void**)hl_malloc(alloc, sizeof(void*) * t->nproto);
		ot->vobj_proto = fptr;
		if( p )
			memcpy(fptr, p->t->vobj_proto, p->nproto * sizeof(void*));
		for(i=0;i<o->nproto;i++) {
			hl_obj_proto *p = o->proto + i;
			if( p->pindex >= 0 ) fptr[p->pindex] = m->functions_ptrs[p->findex];
		}
	}

	t->methods = (void**)hl_malloc(alloc, sizeof(void*) * o->nproto);
	for(i=0;i<o->nproto;i++) {
		hl_obj_proto *p = o->proto + i;
		t->methods[i] = m->functions_ptrs[p->findex];
	}

	return t;
}

void hl_init_virtual( hl_type *vt, hl_module_context *ctx ) {
	int i;
	hl_field_lookup *l = (hl_field_lookup*)hl_malloc(&ctx->alloc,sizeof(hl_field_lookup)*vt->virt->nfields);
	for(i=0;i<vt->virt->nfields;i++) {
		hl_obj_field *f = vt->virt->fields + i;
		hl_lookup_insert(l,i,f->hashed_name,f->t,i);
	}
	vt->vobj_proto = (void**)l;
}

/**
	Allocate a virtual fields mapping to a given value.
**/
vvirtual *hl_to_virtual( hl_type *vt, vdynamic *obj ) {
	vvirtual *v = NULL;
	if( obj == NULL ) return NULL;
#ifdef _DEBUG
	if( vt->vobj_proto == NULL ) hl_fatal("virtual not initialized");
#endif
	switch( obj->t->kind ) {
	case HOBJ:
		{ 
			int i;
			v = (vvirtual*)hl_gc_alloc(sizeof(vvirtual));
			v->t = vt;
			v->fields_data = (char*)obj;
			v->indexes = (int*)hl_gc_alloc(sizeof(int)*vt->virt->nfields);
			v->value = obj;
			v->next = NULL;
			for(i=0;i<vt->virt->nfields;i++) {
				hl_field_lookup *f = obj_resolve_field(obj->t->obj,vt->virt->fields[i].hashed_name);
				if( f && f->field_index < 0 ) {
					hl_type tmp;
					hl_type_fun tf;
					tmp.kind = HFUN;
					tmp.fun = &tf;
					tf.args = f->t->fun->args + 1;
					tf.nargs = f->t->fun->nargs - 1;
					tf.ret = f->t->fun->ret;
					v->indexes[i] = hl_same_type(&tmp,vt->virt->fields[i].t) ? f->field_index : 0;
				} else
					v->indexes[i] = f == NULL || !hl_same_type(f->t,vt->virt->fields[i].t) ? 0 : f->field_index;
			}
		}
		break;
	case HDYNOBJ:
		{
			int i;
			vdynobj *o = (vdynobj*)obj;
			v = o->virtuals;
			while( v ) {
				if( v->t->virt == vt->virt )
					return v;
				v = v->next;
			}
			// allocate a new virtual mapping
			v = (vvirtual*)hl_gc_alloc(sizeof(vvirtual));
			v->t = vt;
			v->fields_data = o->fields_data - sizeof(void*);
			v->indexes = hl_gc_alloc(sizeof(int)*vt->virt->nfields);
			v->value = obj;
			for(i=0;i<vt->virt->nfields;i++) {
				hl_field_lookup *f = hl_lookup_find(&o->dproto->fields,o->nfields,vt->virt->fields[i].hashed_name);
				v->indexes[i] = f == NULL || !hl_same_type(f->t,vt->virt->fields[i].t) ? 0 : f->field_index + sizeof(void*);
			}
			// add it to the list
			v->next = o->virtuals;
			o->virtuals = v;
		}
		break;
	case HVIRTUAL:
		return hl_to_virtual(vt,((vvirtual*)obj)->value);
	default:
		hl_fatal_fmt("Don't know how to virtual %d",obj->t->kind);
	}
	return v;
}

static hl_field_lookup *hl_dyn_alloc_field( vdynobj *o, int hfield, hl_type *t ) {
	int pad = hl_pad_size(o->dataSize, t);
	int size = hl_type_size(t);
	int index;
	char *newData = (char*)hl_gc_alloc(o->dataSize + pad + size);
	vdynobj_proto *proto = (vdynobj_proto*)hl_gc_alloc(sizeof(vdynobj_proto) + sizeof(hl_field_lookup) * (o->nfields + 1 - 1));
	int field_pos = hl_lookup_find_index(&o->dproto->fields, o->nfields, hfield);
	hl_field_lookup *f;
	// update data
	memcpy(newData,o->fields_data,o->dataSize);
	o->fields_data = newData;
	o->dataSize += pad;
	index = o->dataSize;
	o->dataSize += size;
	// update field table
	proto->t = o->dproto->t;
	memcpy(&proto->fields,&o->dproto->fields,field_pos * sizeof(hl_field_lookup));
	f = (&proto->fields) + field_pos;
	f->t = t;
	f->hashed_name = hfield;
	f->field_index = index;
	memcpy(&proto->fields + (field_pos + 1),&o->dproto->fields + field_pos, (o->nfields - field_pos) * sizeof(hl_field_lookup));
	o->nfields++;
	o->dproto = proto;
	// rebuild virtuals
	{
		vvirtual *v = o->virtuals;
		while( v ) {
			hl_field_lookup *vf = hl_lookup_find((hl_field_lookup*)v->t->vobj_proto,v->t->virt->nfields,hfield);
			if( vf && hl_same_type(vf->t,t) )
				v->indexes[vf->field_index] = f->field_index + sizeof(void*);
			v->fields_data = newData - sizeof(void*);
			v = v->next;
		}
	}
	return f;
}

static void hl_dyn_change_field( vdynobj *o, hl_field_lookup *f, hl_type *t ) {
	int i, index, total_size;
	int size = hl_type_size(t);
	int old_size = hl_type_size(f->t);
	char *newData;
	if( size <= old_size ) {
		// don't remap, let's keep hole
		f->t = t;
	} else {
		int wsize = 0;
		index = hl_lookup_find_index(&o->dproto->fields,o->nfields,f->hashed_name);
		total_size = f->field_index;
		for(i=index+1;i<o->nfields;i++) {
			hl_field_lookup *f = &o->dproto->fields + i;
			int fsize = hl_type_size(f->t);
			total_size += hl_pad_size(total_size,f->t);
			memcpy(o->fields_data + total_size, o->fields_data + f->field_index, fsize);
			f->field_index = total_size;
			total_size += fsize;
		}
		wsize = total_size;
		total_size += hl_pad_size(total_size,t);
		f->field_index = total_size;
		f->t = t;
		total_size += size;
		if( total_size > o->dataSize ) {
			newData = (char*)hl_gc_alloc(total_size);
			memcpy(newData,o->fields_data,wsize);
			o->fields_data = newData;
			o->dataSize = total_size;
		}
	}
	// rebuild virtuals
	{
		vvirtual *v = o->virtuals;
		while( v ) {
			hl_field_lookup *vf = hl_lookup_find((hl_field_lookup*)v->t->vobj_proto,v->t->virt->nfields,f->hashed_name);
			if( vf )
				v->indexes[vf->field_index] = hl_same_type(vf->t,t) ? f->field_index + sizeof(void*) : 0;
			v->fields_data = o->fields_data - sizeof(void*);
			v = v->next;
		}
	}
}

// -------------------- DYNAMIC GET ------------------------------------

static void *hl_obj_lookup( vdynamic *d, int hfield, hl_type **t ) {
	switch( d->t->kind ) {
	case HDYNOBJ:
		{
			vdynobj *o = (vdynobj*)d;
			hl_field_lookup *f = hl_lookup_find(&o->dproto->fields,o->nfields,hfield);
			if( f == NULL ) return NULL;
			*t = f->t;
			return o->fields_data + f->field_index;
		}
		break;
	case HOBJ:
		{
			hl_field_lookup *f = obj_resolve_field(d->t->obj,hfield);
			if( f == NULL || f->field_index < 0 ) return NULL;
			*t = f->t;
			return (char*)d + f->field_index;
		}
		break;
	case HVIRTUAL:
		return hl_obj_lookup(((vvirtual*)d)->value, hfield, t);
	default:
		hl_error("Invalid field access");
		break;
	}
	return NULL;
}

// fetch method or dynamic field (getField)
static vdynamic *hl_obj_lookup_extra( vdynamic *d, int hfield ) {
	switch( d->t->kind ) {
	case HOBJ:
		{
			hl_runtime_obj *rt = d->t->obj->rt;
			hl_field_lookup *f = NULL;
			do {
				f = hl_lookup_find(rt->lookup,rt->nlookup,hfield);
				if( f != NULL ) break;
				rt = rt->parent;
			} while( rt );
			if( f && f->field_index < 0 )
				return (vdynamic*)hl_alloc_closure_ptr(f->t,rt->methods[-f->field_index-1],d);
			if( f == NULL ) {
				rt = d->t->obj->rt;
				if( rt->getFieldFun )
					return rt->getFieldFun(d,hfield);
			}
			return NULL;
		}
		break;
	case HVIRTUAL:
		return hl_obj_lookup_extra(((vvirtual*)d)->value, hfield);
	default:
		break;
	}
	return NULL;
}

int hl_dyn_geti( vdynamic *d, int hfield, hl_type *t ) {
	hl_type *ft;
	void *addr = hl_obj_lookup(d,hfield,&ft);
	if( !addr ) return 0;
	switch( ft->kind ) {
	case HI8:
		return *(char*)addr;
	case HI16:
		return *(short*)addr;
	case HI32:
		return *(int*)addr;
	case HF32:
		return (int)*(float*)addr;
	case HF64:
		return (int)*(double*)addr;
	case HBOOL:
		return *(bool*)addr;
	default:
		return hl_dyn_casti(addr,ft,t);
	}
}

float hl_dyn_getf( vdynamic *d, int hfield ) {
	hl_type *ft;
	void *addr = hl_obj_lookup(d,hfield,&ft);
	if( !addr ) return 0.;
	return ft->kind == HF32 ? *(float*)addr : hl_dyn_castf(addr,ft);
}

double hl_dyn_getd( vdynamic *d, int hfield ) {
	hl_type *ft;
	void *addr = hl_obj_lookup(d,hfield,&ft);
	if( !addr ) return 0.;
	return ft->kind == HF64 ? *(double*)addr : hl_dyn_castd(addr,ft);
}

void *hl_dyn_getp( vdynamic *d, int hfield, hl_type *t ) {
	hl_type *ft;
	void *addr = hl_obj_lookup(d,hfield,&ft);
	if( !addr ) {
		d = hl_obj_lookup_extra(d,hfield);
		return d == NULL ? NULL : hl_dyn_castp(&d,&hlt_dyn,t);
	}
	return hl_same_type(t,ft) ? *(void**)addr : hl_dyn_castp(addr,ft,t);
}

// -------------------- DYNAMIC SET ------------------------------------

static void *hl_obj_lookup_set( vdynamic *d, int hfield, hl_type *t, hl_type **ft ) {
	switch( d->t->kind ) {
	case HDYNOBJ:
		{
			vdynobj *o = (vdynobj*)d;
			hl_field_lookup *f = hl_lookup_find(&o->dproto->fields,o->nfields,hfield);
			if( f == NULL )
				f = hl_dyn_alloc_field(o,hfield,t);
			else if( !hl_same_type(t,f->t) )
				hl_dyn_change_field(o,f,t);
			*ft = f->t;
			return o->fields_data + f->field_index;
		}
		break;
	case HOBJ:
		{
			hl_field_lookup *f = obj_resolve_field(d->t->obj,hfield);
			if( f == NULL || f->field_index < 0 ) hl_error_msg(USTR("%s does not have field %s"),d->t->obj->name,hl_field_name(hfield));
			*ft = f->t;
			return (char*)d+f->field_index;
		}
		break;
	case HVIRTUAL:
		return hl_obj_lookup_set(d, hfield, t, ft);
	default:
		hl_error("Invalid field access");
		break;
	}
	return NULL;
}

void hl_dyn_seti( vdynamic *d, int hfield, hl_type *t, int value ) {
	hl_type *ft;
	void *addr = hl_obj_lookup_set(d,hfield,t,&ft);
	switch( ft->kind ) {
	case HI8:
		*(char*)addr = (char)value;
		break;
	case HI16:
		*(short*)addr = (short)value;
		break;
	case HI32:
		*(int*)addr = value;
		break;
	case HBOOL:
		*(bool*)addr = (bool)value;
		break;
	case HF32:
		*(float*)addr = (float)value;
		break;
	case HF64:
		*(double*)addr = value;
		break;
	default:
		{
			vdynamic tmp;
			tmp.t = t;
			tmp.v.i = value;
			hl_write_dyn(addr,ft,&tmp);
		}
		break;
	}
}

void hl_dyn_setf( vdynamic *d, int hfield, float value ) {
	hl_type *t;
	void *addr = hl_obj_lookup_set(d,hfield,&hlt_f32,&t);
	if( t->kind == HF32 )
		*(float*)addr = value;
	else {
		vdynamic tmp;
		tmp.t = &hlt_f32;
		tmp.v.f = value;
		hl_write_dyn(addr,t,&tmp);
	}
}

void hl_dyn_setd( vdynamic *d, int hfield, double value ) {
	hl_type *t;
	void *addr = hl_obj_lookup_set(d,hfield,&hlt_f64,&t);
	if( t->kind == HF64 )
		*(double*)addr = value;
	else {
		vdynamic tmp;
		tmp.t = &hlt_f64;
		tmp.v.d = value;
		hl_write_dyn(addr,t,&tmp);
	}
}

void hl_dyn_setp( vdynamic *d, int hfield, hl_type *t, void *value ) {
	hl_type *ft;
	void *addr = hl_obj_lookup_set(d,hfield,t,&ft);
	if( hl_same_type(t,ft) || value == NULL )
		*(void**)addr = value;
	else if( hl_is_dynamic(t) )
		hl_write_dyn(addr,ft,(vdynamic*)value);
	else {
		vdynamic tmp;
		tmp.t = t;
		tmp.v.ptr = value;
		hl_write_dyn(addr,ft,&tmp);
	}
}

// -------------------- HAXE API ------------------------------------

HL_PRIM vdynamic *hl_obj_get_field( vdynamic *obj, int hfield ) {
	if( obj == NULL )
		return NULL;
	switch( obj->t->kind ) {
	case HOBJ:
	case HVIRTUAL:
	case HDYNOBJ:
		return (vdynamic*)hl_dyn_getp(obj,hfield,&hlt_dyn);
	default:
		return NULL;
	}
}

HL_PRIM void hl_obj_set_field( vdynamic *obj, int hfield, vdynamic *v ) {
	if( v == NULL ) {
		hl_dyn_setp(obj,hfield,&hlt_dyn,NULL);
		return;
	}
	switch( v->t->kind ) {
	case HI8:
		hl_dyn_seti(obj,hfield,v->t,v->v.c);
		break;
	case HI16:
		hl_dyn_seti(obj,hfield,v->t,v->v.s);
		break;
	case HI32:
		hl_dyn_seti(obj,hfield,v->t,v->v.i);
		break;
	case HBOOL:
		hl_dyn_seti(obj,hfield,v->t,v->v.b);
		break;
	case HF32:
		hl_dyn_setf(obj,hfield,v->v.f);
		break;
	case HF64:
		hl_dyn_setd(obj,hfield,v->v.d);
		break;
	default:
		hl_dyn_setp(obj,hfield,v->t,hl_is_dynamic(v->t)?v:v->v.ptr);
		break;
	}
}

HL_PRIM bool hl_obj_has_field( vdynamic *obj, int hfield ) {
	if( obj == NULL ) return false;
	switch( obj->t->kind ) {
	case HOBJ:
		{
			hl_field_lookup *l = obj_resolve_field(obj->t->obj, hfield);
			return l && l->field_index >= 0;
		}
		break;
	case HDYNOBJ:
		{
			vdynobj *d = (vdynobj*)obj;
			hl_field_lookup *f = hl_lookup_find(&d->dproto->fields,d->nfields,hfield);
			return f != NULL;
		}
		break;
	case HVIRTUAL:
		return hl_obj_has_field(((vvirtual*)obj)->value,hfield);
	default:
		break;
	}
	return false;
}

HL_PRIM bool hl_obj_delete_field( vdynamic *obj, int hfield ) {
	switch( obj->t->kind ) {
	case HDYNOBJ:
		{
			vdynobj *d = (vdynobj*)obj;
			hl_field_lookup *f = hl_lookup_find(&d->dproto->fields,d->nfields,hfield);
			if( f == NULL ) return false;
			memcpy(f, f + 1, ((char*)(&d->dproto->fields + d->nfields)) - (char*)(f + 1));
			d->nfields--;
			// rebuild virtuals
			{
				vvirtual *v = d->virtuals;
				while( v ) {
					hl_field_lookup *vf = hl_lookup_find((hl_field_lookup*)v->t->vobj_proto,v->t->virt->nfields,hfield);
					if( vf )
						v->indexes[vf->field_index] = 0;
					v = v->next;
				}
			}
			return true;
		}
		break;
	case HVIRTUAL:
		return hl_obj_delete_field(((vvirtual*)obj)->value,hfield);
	default:
		break;
	}
	return false;
}

HL_PRIM varray *hl_obj_fields( vdynamic *obj ) {
	varray *a = NULL;
	switch( obj->t->kind ) {
	case HDYNOBJ:
		{
			vdynobj *o = (vdynobj*)obj;
			int i;
			a = hl_aalloc(&hlt_bytes,o->nfields);
			for(i=0;i<o->nfields;i++)
				((vbyte**)(a + 1))[i] = (vbyte*)hl_field_name((&o->dproto->fields + i)->hashed_name);
		}
		break;
	case HOBJ:
		{
			hl_type_obj *tobj = obj->t->obj;
			hl_runtime_obj *o = tobj->rt;
			int i, p = 0;
			a = hl_aalloc(&hlt_bytes,o->nfields);
			while( true ) {
				for(i=0;i<tobj->nfields;i++) {
					hl_obj_field *f = tobj->fields + i;
					((vbyte**)(a + 1))[p++] =  (vbyte*)f->name;
				}
				if( tobj->super == NULL ) break;
				tobj = tobj->super->obj;
			}
		}
		break;
	default:
		break;
	}
	return a;
}

HL_PRIM vdynamic *hl_obj_copy( vdynamic *obj ) {
	if( obj == NULL )
		return NULL;
	switch( obj->t->kind ) {
	case HDYNOBJ:
		{
			vdynobj *o = (vdynobj*)obj;
			vdynobj *c = hl_alloc_dynobj();
			int protoSize = sizeof(vdynobj_proto) + sizeof(hl_field_lookup) * (o->nfields + 1 - 1);
			c->dataSize = o->dataSize;
			c->nfields = o->nfields;
			c->virtuals = NULL;
			c->dproto = (vdynobj_proto*)hl_gc_alloc(protoSize);
			memcpy(c->dproto,o->dproto,protoSize);
			c->fields_data = (char*)hl_gc_alloc(o->dataSize);
			memcpy(c->fields_data,o->fields_data,o->dataSize);
			return (vdynamic*)c;
		}
		break;
	default:
		break;
	}
	return NULL;
}