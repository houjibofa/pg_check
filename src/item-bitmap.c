#include "item-bitmap.h"

#include <unistd.h>

#include "access/itup.h"

/* allocate the memory in 1kB chunks */
#define PALLOC_CHUNK 1024

/* allocate the bitmap (~4.5MB for each 1GB of heap) */
item_bitmap * bitmap_alloc(int npages) {
	
	item_bitmap * bitmap = (item_bitmap*)palloc(sizeof(item_bitmap));
	
	bitmap->npages = npages;
	bitmap->pages = (int*)palloc(sizeof(int)*npages);
	
	bitmap->nbytes = 0;
	bitmap->maxBytes = PALLOC_CHUNK;
	bitmap->data = (char*)palloc(PALLOC_CHUNK);
	
	return bitmap;
	
}

/* allocate the bitmap (~4.5MB for each 1GB of heap) */
item_bitmap * bitmap_prealloc(item_bitmap * src) {
	
	item_bitmap * bitmap = (item_bitmap*)palloc(sizeof(item_bitmap));
	
	bitmap->npages = src->npages;
	
	bitmap->nbytes = src->nbytes;
	bitmap->maxBytes = src->maxBytes;

	bitmap->pages = (int*)palloc(sizeof(int)*src->npages);
	memcpy(bitmap->pages, src->pages, sizeof(int)*src->npages);
	
	bitmap->data = (char*)palloc(src->maxBytes);
	memset(bitmap->data, 0, src->maxBytes);
	
	return bitmap;
	
}

void bitmap_reset(item_bitmap* bitmap) {
	memset(bitmap->data, 0, bitmap->maxBytes);
}

void bitmap_free(item_bitmap* bitmap) {
	pfree(bitmap->pages);
	pfree(bitmap->data);
	pfree(bitmap);
}

/* needs to be called for paged 0,1,2,3,...npages (not randomly) */
/* extends the bitmap to handle another page */
void bitmap_add_page(item_bitmap * bitmap, int page, int items) {

	bitmap->pages[page] = (page == 0) ? items : (items + bitmap->pages[page-1]);
	
	/* if needed more bytes than */
	bitmap->nbytes = ((bitmap->pages[page] + 7) / 8);
	if (bitmap->nbytes > bitmap->maxBytes) {
		bitmap->maxBytes += PALLOC_CHUNK;
		bitmap->data = (char*)repalloc(bitmap->data, bitmap->maxBytes);
	}

}

/* update the bitmap bith all items from a page (tracks number of items) */
int bitmap_add_heap_items(item_bitmap * bitmap, PageHeader header, char *raw_page, int page) {

	/* tuple checks */
	int nerrs = 0;
	int ntuples = PageGetMaxOffsetNumber(raw_page);
	int item;
	
	bitmap_add_page(bitmap, page, ntuples);
	
	/* by default set all LP_REDIRECT / LP_NORMAL items to '1' (we'll remove the HOT chains in the second pass) */
	/* FIXME what if there is a HOT chain and then an index is created? */
	for (item = 0; item < ntuples; item++) {
		if ((header->pd_linp[item].lp_flags == LP_NORMAL) || (header->pd_linp[item].lp_flags == LP_REDIRECT)) {
			if (! bitmap_set_item(bitmap, page, item, true)) {
				nerrs++;
			}
		}
	}
	
	/* second pass - remove the HOT chains */
	for (item = 0; item < ntuples; item++) {
	
		if (header->pd_linp[item].lp_flags == LP_REDIRECT) {
			/* walk only if not walked this HOT chain yet (skip the first item in the chain) */
			if (bitmap_get_item(bitmap, page, item)) {
				int next = item;
				do {
					next = header->pd_linp[next].lp_off;
					if (! bitmap_set_item(bitmap, page, next, false)) {
						nerrs++;
					}
				} while (header->pd_linp[next].lp_flags != LP_REDIRECT);
			}
		}
		
	}
	
	return nerrs;

}

/* checks index tuples on the page, one by one */
int bitmap_add_index_items(item_bitmap * bitmap, PageHeader header, char *raw_page, int page) {

	/* tuple checks */
	int nerrs = 0;
	int ntuples = PageGetMaxOffsetNumber(raw_page);
	int item;
	
	for (item = 0; item < ntuples; item++) {

		IndexTuple itup = (IndexTuple)(raw_page + header->pd_linp[item].lp_off);
		if (! bitmap_set_item(bitmap, BlockIdGetBlockNumber(&(itup->t_tid.ip_blkid)), (itup->t_tid.ip_posid-1), true)) {
			nerrs++;
		}
		
	}
	
	return nerrs;
  
}

/* mark the (page,item) as occupied */
bool bitmap_set_item(item_bitmap * bitmap, int page, int item, bool state) {

	int byteIdx = (GetBitmapIndex(bitmap, page, item)) / 8;
	int bitIdx  = (GetBitmapIndex(bitmap, page, item)) % 8;
	
	if (page >= bitmap->npages) {
		elog(WARNING, "invalid page %d (max page %d)", page, bitmap->npages-1);
		return false;
	}
	
	if (byteIdx > bitmap->nbytes) {
		elog(WARNING, "invalid byte %d (max byte %d)", byteIdx, bitmap->nbytes);
		return false;
	}
	
	if (item >= bitmap->pages[page]) {
		elog(WARNING, "item %d out of range, page has only %d items", item, bitmap->pages[page]);
		return false;
	}

	if (state) {
		/* set the bit (OR) */
		bitmap->data[byteIdx] |= (1 << bitIdx);
	} else {
		/* remove the bit (XOR) */
		bitmap->data[byteIdx] &= ~(1 << bitIdx);
	}
	
	return true;

}

/* check if the (page,item) is occupied */
bool bitmap_get_item(item_bitmap * bitmap, int page, int item) {

	int byteIdx = (GetBitmapIndex(bitmap, page, item)) / 8;
	int bitIdx  = (GetBitmapIndex(bitmap, page, item)) % 8;
	
	if (page >= bitmap->npages) {
		elog(WARNING, "invalid page %d (max page %d)", page, bitmap->npages-1);
		return false;
	}
	
	if (byteIdx > bitmap->nbytes) {
		elog(WARNING, "invalid byte %d (max byte %d)", byteIdx, bitmap->nbytes);
		return false;
	}
	
	if (item >= bitmap->pages[page]) {
		elog(WARNING, "item %d out of range, page has only %d items", item, bitmap->pages[page]);
		return false;
	}

	return (bitmap->data[byteIdx] && (1 << bitIdx));

}

/* counts bits set to 1 in the bitmap */
long bitmap_count(item_bitmap * bitmap) {

	long i, j, items = 0;
	
	for (i = 0; i < bitmap->nbytes; i++) {
		for (j = 0; j < 8; j++) {
			if (bitmap->data[i] & (1 << j)) {
				items++;
			}
		}
	}
	
	return items;
	
}

/* compare bitmaps, returns number of differences */
long bitmap_compare(item_bitmap * bitmap_a, item_bitmap * bitmap_b) {

	long i, j, ndiff = 0;
	char diff = 0;

	/* compare number of pages and total items */
	/* FIXME this rather a sanity check, because these values are copied by bitmap_prealloc */
	if (bitmap_a->npages != bitmap_b->npages) {
		elog(WARNING, "bitmaps do not track the same number of pages (%d != %d)",
			 bitmap_a->npages, bitmap_b->npages);
		return MAX(bitmap_a->pages[bitmap_a->npages-1],
				   bitmap_b->pages[bitmap_b->npages-1]);
	} else if (bitmap_a->pages[bitmap_a->npages-1] != bitmap_b->pages[bitmap_b->npages-1]) {
		elog(WARNING, "bitmaps do not track the same number of pages (%d != %d)",
			 bitmap_a->pages[bitmap_a->npages-1], bitmap_b->pages[bitmap_b->npages-1]);
	}
	
	/* the actual check, compares the bits one by one */
	for (i = 0; i < bitmap_a->nbytes; i++) {
		diff = (bitmap_a->data[i] ^ bitmap_b->data[i]);
		if (diff != 0) {
			for (j = 0; j < 8; j++) {
				if (diff & (1 << j)) {
					ndiff++;
				}
			}
		}
	}
	
	return ndiff;
	
}

/* print the bitmap (for debugging purposes) */
/* TODO print details about differences (items missing in heap, items missing in index) */
void bitmap_print(item_bitmap * bitmap) {

	int i, j, k = 0;
	char str[bitmap->nbytes*8+1];
	
	for (i = 0; i < bitmap->nbytes; i++) {
		for (j = 0; j < 8; j++) {
			if (bitmap->data[i] & (1 << j)) {
				str[k++] = '1';
			} else {
				str[k++] = '0';
			}
		}
	}
	str[k++] = 0;
	elog(WARNING, "bitmap [%s]", str);
}