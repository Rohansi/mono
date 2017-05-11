#include "unity_memory_info.h"
#include <mono/metadata/assembly.h>
#include <mono/metadata/class.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/image.h>
#include <mono/metadata/metadata-internals.h>
#include <mono/metadata/object-internals.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/tokentype.h>
#include <stdlib.h>
#include <libgc/include/gc.h>
#include <libgc/include/private/gc_priv.h>
#include <mono/metadata/gc-internal.h>

#include <glib.h>

typedef struct CollectMetadataContext
{
	GHashTable *allTypes;
	int currentIndex;
	MonoMetadataSnapshot* metadata;
} CollectMetadataContext;

static void CollectAssemblyMetaData (MonoAssembly *assembly, void *user_data)
{
	int i;
	CollectMetadataContext* context = (CollectMetadataContext*)user_data;
	MonoImage* image = mono_assembly_get_image(assembly);
	MonoTableInfo *tdef = &image->tables [MONO_TABLE_TYPEDEF];

	for(i = 0; i < tdef->rows-1; ++i)
	{
		MonoClass* klass = mono_class_get (image, (i + 2) | MONO_TOKEN_TYPE_DEF);

		if(klass->inited)
			g_hash_table_insert(context->allTypes, klass, (gpointer)(context->currentIndex++));
	}
}

static int FindClassIndex(GHashTable* hashTable, MonoClass* klass)
{
	gpointer value = g_hash_table_lookup(hashTable, klass);

	if(!value)
		return -1;

	return (int)value;
}

static gchar* GetTypeName(MonoClass* klass)
{
	const char* name = mono_class_get_name(klass);
	const char* name_space = mono_class_get_namespace(klass);

	return g_strdup_printf("%s.%s", name_space, name);
}

static void AddMetadataType (gpointer key, gpointer value, gpointer user_data)
{
	MonoClass* klass = (MonoClass*)key;
	int index = (int)value;
	CollectMetadataContext* context = (CollectMetadataContext*)user_data;
	MonoMetadataSnapshot* metadata = context->metadata;
	MonoMetadataType* type = &metadata->types[index];

	if(klass->rank > 0)
	{
		type->flags = (MonoMetadataTypeFlags)(kArray | (kArrayRankMask & (klass->rank << 16)));
		type->baseOrElementTypeIndex = FindClassIndex(context->allTypes, mono_class_get_element_class(klass));
	}
	else
	{
		gpointer iter = NULL;
		int fieldCount = 0;
		MonoClassField* field;
		MonoClass* baseClass;
		MonoVTable* vtable;

        type->flags = (klass->valuetype || klass->byval_arg.type == MONO_TYPE_PTR) ? kValueType : kNone;
		type->fieldCount = 0;

		if(mono_class_num_fields(klass) > 0)
		{
			type->fields = g_new(MonoMetadataField, mono_class_num_fields(klass));

			while ((field = mono_class_get_fields (klass, &iter))) 
			{
				MonoMetadataField* metaField = &type->fields[type->fieldCount];
				metaField->typeIndex = FindClassIndex(context->allTypes, mono_class_from_mono_type(field->type));

				// This will happen if fields type is not initialized
                // It's OK to skip it, because it means the field is guaranteed to be null on any object
				if (metaField->typeIndex == -1)
					continue;

				// literals have no actual storage, and are not relevant in this context.
				if((field->type->attrs & FIELD_ATTRIBUTE_LITERAL) != 0)
					continue;

                metaField->isStatic = (field->type->attrs & FIELD_ATTRIBUTE_STATIC) != 0;
                metaField->offset = field->offset;
                metaField->name = field->name;
                type->fieldCount++;
			}
		}

		vtable = mono_class_try_get_vtable(mono_domain_get(), klass);

		type->staticsSize = vtable ? mono_class_data_size(klass) : 0; // Correct?
		type->statics = NULL;

		if (type->staticsSize > 0 && vtable && vtable->data)
		{
			type->statics = g_new0(uint8_t, type->staticsSize);
			memcpy(type->statics, vtable->data, type->staticsSize);
		}

		baseClass = mono_class_get_parent(klass);
		type->baseOrElementTypeIndex = baseClass ? FindClassIndex(context->allTypes, baseClass) : -1;
	}

	type->assemblyName = mono_class_get_image(klass)->assembly->aname.name;
	type->name = GetTypeName(klass);
	type->typeInfoAddress = (uint64_t)klass;
	type->size = (klass->valuetype) != 0 ? (mono_class_instance_size(klass) - sizeof(MonoObject)) : mono_class_instance_size(klass);
}


static void CollectMetadata(MonoMetadataSnapshot* metadata)
{
	CollectMetadataContext context;

	context.allTypes = g_hash_table_new(NULL, NULL);
	context.currentIndex = 0;
	context.metadata = metadata;
	
	mono_assembly_foreach((GFunc)CollectAssemblyMetaData, &context);

	metadata->typeCount = g_hash_table_size(context.allTypes);
	metadata->types = g_new0(MonoMetadataType, metadata->typeCount);

	g_hash_table_foreach(context.allTypes, AddMetadataType, &context);

	g_hash_table_destroy(context.allTypes);
}

static struct hblk* GetNextFreeBlock(ptr_t ptr)
{
	struct hblk* result = NULL;
	unsigned i;

	for (i = 0; i < N_HBLK_FLS + 1; i++)
	{
		struct hblk* freeBlock = GC_hblkfreelist[i];
		
		for (freeBlock = GC_hblkfreelist[i]; freeBlock != NULL; freeBlock = HDR(freeBlock)->hb_next)
		{
			/* We're only interested in pointers after "ptr" argument */
			if ((ptr_t)freeBlock < ptr)
				continue;

			/* If we haven't had a result before or our previous result is */
			/* ahead of the current freeBlock, mark the current freeBlock as result */
			if (result == NULL || result > freeBlock)
				result = freeBlock;
		}
	}

	return result;
}

typedef void (*GC_heap_section_proc)(void* user_data, GC_PTR start, GC_PTR end);

void GC_foreach_heap_section(void* user_data, GC_heap_section_proc callback)
{
	unsigned i;
	
	GC_ASSERT(I_HOLD_LOCK());

	if (callback == NULL)
		return;

	for (i = 0; i < GC_n_heap_sects; i++)
	{
		ptr_t sectionStart = GC_heap_sects[i].hs_start;
		ptr_t sectionEnd = sectionStart + GC_heap_sects[i].hs_bytes;

		while (sectionStart < sectionEnd)
		{
			struct hblk* nextFreeBlock = GetNextFreeBlock(sectionStart);
			
			if (nextFreeBlock == NULL || (ptr_t)nextFreeBlock > sectionEnd)
			{
				callback(user_data, sectionStart, sectionEnd);
				break;
			}
			else
			{
				size_t sectionLength = (char*)nextFreeBlock - sectionStart;

				if (sectionLength > 0)
					callback(user_data, sectionStart, sectionStart + sectionLength);

				sectionStart = (char*)nextFreeBlock + HDR(nextFreeBlock)->hb_sz;
			}
		}
	}
}

static void HeapSectionCountIncrementer(void* context, GC_PTR start, GC_PTR end)
{
	GC_word* countPtr = (GC_word*)context;
	(*countPtr)++;
}

GC_word GC_get_heap_section_count()
{
	GC_word count = 0;
	GC_foreach_heap_section(&count, HeapSectionCountIncrementer);
	return count;
}

typedef struct SectionIterationContext
{
    MonoManagedMemorySection* currentSection;
} SectionIterationContext;

static void AllocateMemoryForSection(void* context, void* sectionStart, void* sectionEnd)
{
	ptrdiff_t sectionSize;

    SectionIterationContext* ctx = (SectionIterationContext*)context;
    MonoManagedMemorySection* section = ctx->currentSection;

    section->sectionStartAddress = (uint64_t)sectionStart;
    sectionSize = (uint8_t*)(sectionEnd) - (uint8_t*)(sectionStart);

    section->sectionSize = (uint32_t)(sectionSize);
    section->sectionBytes = (uint8_t)(g_new(uint8_t, section->sectionSize));

    ctx->currentSection++;
}

static void CopyHeapSection(void* context, void* sectionStart, void* sectionEnd)
{
    SectionIterationContext* ctx = (SectionIterationContext*)(context);
    MonoManagedMemorySection* section = ctx->currentSection;

    g_assert(section->sectionStartAddress == (uint64_t)(sectionStart));
    g_assert(section->sectionSize == (uint8_t*)(sectionEnd) - (uint8_t*)(sectionStart));
    memcpy(section->sectionBytes, sectionStart, section->sectionSize);

    ctx->currentSection++;
}

static void* CaptureHeapInfo(void* voidManagedHeap)
{
    MonoManagedHeap* heap = (MonoManagedHeap*)voidManagedHeap;
	SectionIterationContext iterationContext;

    heap->sectionCount = GC_get_heap_section_count();
    heap->sections = g_new0(MonoManagedMemorySection, heap->sectionCount);

	iterationContext.currentSection = heap->sections;

	GC_foreach_heap_section(&iterationContext, AllocateMemoryForSection);

    return NULL;
}

static void FreeMonoManagedHeap(MonoManagedHeap* heap)
{
	uint32_t i;

    for (i = 0; i < heap->sectionCount; i++)
    {
        g_free(heap->sections[i].sectionBytes);
    }

    g_free(heap->sections);
}

typedef struct VerifyHeapSectionStillValidIterationContext
{
    MonoManagedMemorySection* currentSection;
    gboolean wasValid;
} VerifyHeapSectionStillValidIterationContext;

static void VerifyHeapSectionIsStillValid(void* context, void* sectionStart, void* sectionEnd)
{
    VerifyHeapSectionStillValidIterationContext* iterationContext = (VerifyHeapSectionStillValidIterationContext*)context;
    if (iterationContext->currentSection->sectionSize != (uint8_t*)(sectionEnd) - (uint8_t*)(sectionStart))
        iterationContext->wasValid = FALSE;
    else if (iterationContext->currentSection->sectionStartAddress != (uint64_t)(sectionStart))
        iterationContext->wasValid = FALSE;

    iterationContext->currentSection++;
}

static gboolean MonoManagedHeapStillValid(MonoManagedHeap* heap)
{
	VerifyHeapSectionStillValidIterationContext iterationContext;

    if (heap->sectionCount != GC_get_heap_section_count())
		return FALSE;

	iterationContext.currentSection = heap->sections;
	iterationContext.wasValid = TRUE;

	GC_foreach_heap_section(&iterationContext, VerifyHeapSectionIsStillValid);
    
	return iterationContext.wasValid;
}

// The difficulty in capturing the managed snapshot is that we need to do quite some work with the world stopped,
// to make sure that our snapshot is "valid", and didn't change as we were copying it. However, stopping the world,
// makes it so you cannot take any lock or allocations. We deal with it like this:
//
// 1) We take note of the amount of heap sections and their sizes, and we allocate memory to copy them into.
// 2) We stop the world.
// 3) We check if the amount of heapsections and their sizes didn't change in the mean time. If they did, try again.
// 4) Now, with the world still stopped, we memcpy() the memory from the real heapsections, into the memory that we
//    allocated for their copies.
// 5) Start the world again.

static void CaptureManagedHeap(MonoManagedHeap* heap)
{
	SectionIterationContext iterationContext;

	while(TRUE)
	{
		GC_call_with_alloc_lock(CaptureHeapInfo, heap);
		GC_stop_world_external();

		if (MonoManagedHeapStillValid(heap))
	        break;

		GC_start_world_external();

		FreeMonoManagedHeap(heap);
	}
	
	iterationContext.currentSection = heap->sections;
    GC_foreach_heap_section(&iterationContext, CopyHeapSection);

	GC_start_world_external();
}

typedef struct GCHandleTargetIterationContext
{
	GList* managedObjects;
} GCHandleTargetIterationContext;

static void GCHandleIterationCallback(MonoObject* managedObject, void* context)
{
    GCHandleTargetIterationContext* ctx = (GCHandleTargetIterationContext*)(context);
	g_list_append(ctx->managedObjects, managedObject);
}

static inline void CaptureGCHandleTargets(MonoGCHandles* gcHandles)
{
	uint32_t i;
	GList* trackedObjects, *trackedObject;

    GCHandleTargetIterationContext gcHandleTargetIterationContext;
	gcHandleTargetIterationContext.managedObjects = g_list_alloc();

	mono_gc_strong_handle_foreach((GFunc)GCHandleIterationCallback, &gcHandleTargetIterationContext);

	trackedObjects = gcHandleTargetIterationContext.managedObjects;

	gcHandles->trackedObjectCount = (uint32_t)g_list_length(trackedObjects);
    gcHandles->pointersToObjects = (uint64_t*)g_new0(uint64_t, gcHandles->trackedObjectCount);

	trackedObject = trackedObjects;

    for (i = 0; i < gcHandles->trackedObjectCount; i++)
	{
		gcHandles->pointersToObjects[i] = (uint64_t)trackedObject->data;
		trackedObject = g_list_next(trackedObject);
	}

	g_list_free(gcHandleTargetIterationContext.managedObjects);
}

static void FillRuntimeInformation(MonoRuntimeInformation* runtimeInfo)
{
    runtimeInfo->pointerSize = (uint32_t)(sizeof(void*));
    runtimeInfo->objectHeaderSize = (uint32_t)(sizeof(MonoObject));
    runtimeInfo->arrayHeaderSize = offsetof(MonoArray, vector);
    runtimeInfo->arraySizeOffsetInHeader = offsetof(MonoArray, max_length);
    runtimeInfo->arrayBoundsOffsetInHeader = offsetof(MonoArray, bounds);
    runtimeInfo->allocationGranularity = (uint32_t)(2 * sizeof(void*));
}

MonoManagedMemorySnapshot* mono_unity_capture_memory_snapshot()
{
	MonoManagedMemorySnapshot* snapshot;
	snapshot = g_new0(MonoManagedMemorySnapshot, 1);

	CollectMetadata(&snapshot->metadata);
	CaptureManagedHeap(&snapshot->heap);
	CaptureGCHandleTargets(&snapshot->gcHandles);
	FillRuntimeInformation(&snapshot->runtimeInformation);

	return snapshot;
}

void mono_unity_free_captured_memory_snapshot(MonoManagedMemorySnapshot* snapshot)
{
	uint32_t i;
    MonoMetadataSnapshot* metadata = &snapshot->metadata;

	FreeMonoManagedHeap(&snapshot->heap);

    g_free(snapshot->gcHandles.pointersToObjects);

    for (i = 0; i < metadata->typeCount; i++)
    {
        if ((metadata->types[i].flags & kArray) == 0)
        {
            g_free(metadata->types[i].fields);
            g_free(metadata->types[i].statics);
        }

		g_free(metadata->types[i].name);
    }

    g_free(metadata->types);
    g_free(snapshot);
}