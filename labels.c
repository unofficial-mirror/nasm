/* labels.c  label handling for the Netwide Assembler
 *
 * The Netwide Assembler is copyright (C) 1996 Simon Tatham and
 * Julian Hall. All rights reserved. The software is
 * redistributable under the licence given in the file "Licence"
 * distributed in the NASM archive.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "nasm.h"
#include "nasmlib.h"

/*
 * A local label is one that begins with exactly one period. Things
 * that begin with _two_ periods are NASM-specific things.
 */
#define islocal(l) ((l)[0] == '.' && (l)[1] != '.')

#define LABEL_BLOCK  320	       /* no. of labels/block */
#define LBLK_SIZE    (LABEL_BLOCK*sizeof(union label))
#define LABEL_HASHES 32		       /* no. of hash table entries */

#define END_LIST -3		       /* don't clash with NO_SEG! */
#define END_BLOCK -2
#define BOGUS_VALUE -4

#define PERMTS_SIZE  4096	       /* size of text blocks */

/* values for label.defn.is_global */
#define NOT_DEFINED_YET 0
#define LOCAL_SYMBOL 1
#define GLOBAL_SYMBOL 2
#define GLOBAL_PLACEHOLDER 3

union label {			       /* actual label structures */
    struct {
	long segment, offset;
        char *label;
	int is_global;
    } defn;
    struct {
	long movingon, dummy;
	union label *next;
    } admin;
};

struct permts {			       /* permanent text storage */
    struct permts *next;	       /* for the linked list */
    int size, usage;		       /* size and used space in ... */
    char data[PERMTS_SIZE];	       /* ... the data block itself */
};

static union label *ltab[LABEL_HASHES];/* using a hash table */
static union label *lfree[LABEL_HASHES];/* pointer into the above */
static struct permts *perm_head;      /* start of perm. text storage */
static struct permts *perm_tail;      /* end of perm. text storage */

static void init_block (union label *blk);
static char *perm_copy (char *string1, char *string2);

static char *prevlabel;

/*
 * Internal routine: finds the `union label' corresponding to the
 * given label name. Creates a new one, if it isn't found, and if
 * `create' is TRUE.
 */
static union label *find_label (char *label, int create) {
    int hash = 0;
    char *p, *prev;
    int prevlen;
    union label *lptr;

    if (islocal(label))
	prev = prevlabel;
    else
	prev = "";
    prevlen = strlen(prev);
    p = prev;
    while (*p) hash += *p++;
    p = label;
    while (*p) hash += *p++;
    hash %= LABEL_HASHES;
    lptr = ltab[hash];
    while (lptr->admin.movingon != END_LIST) {
	if (lptr->admin.movingon == END_BLOCK) {
	    lptr = lptr->admin.next;
	    if (!lptr)
		break;
	}
	if (!strncmp(lptr->defn.label, prev, prevlen) &&
	    !strcmp(lptr->defn.label+prevlen, label))
	    return lptr;
	lptr++;
    }
    if (create) {
	if (lfree[hash]->admin.movingon == END_BLOCK) {
	    /*
	     * must allocate a new block
	     */
	    lfree[hash]->admin.next = (union label *) nasm_malloc (LBLK_SIZE);
	    lfree[hash] = lfree[hash]->admin.next;
	    init_block(lfree[hash]);
	}

	lfree[hash]->admin.movingon = BOGUS_VALUE;
	lfree[hash]->defn.label = perm_copy (prev, label);
	lfree[hash]->defn.is_global = NOT_DEFINED_YET;
	return lfree[hash]++;
    } else
	return NULL;
}

int lookup_label (char *label, long *segment, long *offset) {
    union label *lptr;

    lptr = find_label (label, 0);
    if (lptr && (lptr->defn.is_global == LOCAL_SYMBOL ||
		 lptr->defn.is_global == GLOBAL_SYMBOL)) {
	*segment = lptr->defn.segment;
	*offset = lptr->defn.offset;
	return 1;
    } else
	return 0;
}

void define_label_stub (char *label, efunc error) {
    union label *lptr;

    if (!islocal(label)) {
	lptr = find_label (label, 1);
	if (!lptr)
	    error (ERR_PANIC, "can't find label `%s' on pass two", label);
	if (*label != '.')
	    prevlabel = lptr->defn.label;
    }
}

void define_label (char *label, long segment, long offset,
		   struct ofmt *ofmt, efunc error) {
    union label *lptr;

    lptr = find_label (label, 1);
    switch (lptr->defn.is_global) {
      case NOT_DEFINED_YET:
	lptr->defn.is_global = LOCAL_SYMBOL;
	break;
      case GLOBAL_PLACEHOLDER:
	lptr->defn.is_global = GLOBAL_SYMBOL;
	break;
      default:
	error(ERR_NONFATAL, "symbol `%s' redefined", label);
	return;
    }

    if (label[0] != '.')	       /* not local, but not special either */
	prevlabel = lptr->defn.label;
    else if (label[1] != '.' && !*prevlabel)
	error(ERR_NONFATAL, "attempt to define a local label before any"
	      " non-local labels");

    lptr->defn.segment = segment;
    lptr->defn.offset = offset;

    ofmt->symdef (lptr->defn.label, segment, offset,
		  lptr->defn.is_global == GLOBAL_SYMBOL);
}

void define_common (char *label, long segment, long size,
		    struct ofmt *ofmt, efunc error) {
    union label *lptr;

    lptr = find_label (label, 1);
    switch (lptr->defn.is_global) {
      case NOT_DEFINED_YET:
	lptr->defn.is_global = LOCAL_SYMBOL;
	break;
      case GLOBAL_PLACEHOLDER:
	lptr->defn.is_global = GLOBAL_SYMBOL;
	break;
      default:
	error(ERR_NONFATAL, "symbol `%s' redefined", label);
	return;
    }

    if (label[0] != '.')	       /* not local, but not special either */
	prevlabel = lptr->defn.label;
    else
	error(ERR_NONFATAL, "attempt to define a local label as a "
	      "common variable");

    lptr->defn.segment = segment;
    lptr->defn.offset = 0;

    ofmt->symdef (lptr->defn.label, segment, size, 2);
}

void declare_as_global (char *label, efunc error) {
    union label *lptr;

    if (islocal(label)) {
	error(ERR_NONFATAL, "attempt to declare local symbol `%s' as"
	      " global", label);
	return;
    }
    lptr = find_label (label, 1);
    switch (lptr->defn.is_global) {
      case NOT_DEFINED_YET:
	lptr->defn.is_global = GLOBAL_PLACEHOLDER;
	break;
      case GLOBAL_PLACEHOLDER:	       /* already done: silently ignore */
      case GLOBAL_SYMBOL:
	break;
      case LOCAL_SYMBOL:
	error(ERR_NONFATAL, "symbol `%s': [GLOBAL] directive must"
	      " appear before symbol definition", label);
	break;
    }
}

int init_labels (void) {
    int i;

    for (i=0; i<LABEL_HASHES; i++) {
	ltab[i] = (union label *) nasm_malloc (LBLK_SIZE);
	if (!ltab[i])
	    return -1;		       /* can't initialise, panic */
	init_block (ltab[i]);
	lfree[i] = ltab[i];
    }

    perm_head = perm_tail = (struct permts *) nasm_malloc (sizeof(struct permts));
    if (!perm_head)
    	return -1;

    perm_head->next = NULL;
    perm_head->size = PERMTS_SIZE;
    perm_head->usage = 0;

    prevlabel = "";

    return 0;
}

void cleanup_labels (void) {
    int i;

    for (i=0; i<LABEL_HASHES; i++) {
	union label *lptr, *lhold;

	lptr = lhold = ltab[i];

	while (lptr) {
	    while (lptr->admin.movingon != END_BLOCK) lptr++;
	    lptr = lptr->admin.next;
	    nasm_free (lhold);
	    lhold = lptr;
	}
    }

    while (perm_head) {
	perm_tail = perm_head;
	perm_head = perm_head->next;
	nasm_free (perm_tail);
    }
}

static void init_block (union label *blk) {
    int j;

    for (j=0; j<LABEL_BLOCK-1; j++)
    	blk[j].admin.movingon = END_LIST;
    blk[LABEL_BLOCK-1].admin.movingon = END_BLOCK;
    blk[LABEL_BLOCK-1].admin.next = NULL;
}

static char *perm_copy (char *string1, char *string2) {
    char *p, *q;
    int len = strlen(string1)+strlen(string2)+1;

    if (perm_tail->size - perm_tail->usage < len) {
	perm_tail->next = (struct permts *)nasm_malloc(sizeof(struct permts));
	perm_tail = perm_tail->next;
	perm_tail->next = NULL;
	perm_tail->size = PERMTS_SIZE;
	perm_tail->usage = 0;
    }
    p = q = perm_tail->data + perm_tail->usage;
    while ( (*q = *string1++) ) q++;
    while ( (*q++ = *string2++) );
    perm_tail->usage = q - perm_tail->data;

    return p;
}
