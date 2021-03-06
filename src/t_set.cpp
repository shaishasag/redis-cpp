/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "fmacros.h"
#include "server.h"
#include "sds.h"

/*-----------------------------------------------------------------------------
 * Set Commands
 *----------------------------------------------------------------------------*/

void sunionDiffGenericCommand(client *c, robj **setkeys, int setnum,
                              robj *dstkey, int op);

/* Factory method to return a set that *can* hold "value". When the object has
 * an integer-encodable value, an intset will be returned. Otherwise a regular
 * hash table. */
robj *setTypeCreate(sds value) {
    if (isSdsRepresentableAsLongLong(value,NULL) == C_OK)
        return createIntsetObject();
    return createSetObject();
}

/* Add the specified value into a set.
 *
 * If the value was already member of the set, nothing is done and 0 is
 * returned, otherwise the new element is added and 1 is returned. */
int setTypeAdd(robj *subject, sds value) {
    long long llval;
    if (subject->encoding == OBJ_ENCODING_HT) {
        dict *ht = (dict *)subject->ptr;
        dictEntry *de = ht->dictAddRaw(value,NULL);
        if (de) {
            ht->dictSetKey(de,sdsdup(value));
            ht->dictSetVal(de,NULL);
            return 1;
        }
    } else if (subject->encoding == OBJ_ENCODING_INTSET) {
        if (isSdsRepresentableAsLongLong(value,&llval) == C_OK) {
            uint8_t success = 0;
            subject->ptr = intset::intsetAdd((intset *)subject->ptr,llval,&success);
            if (success) {
                /* Convert to regular set when the intset contains
                 * too many entries. */
                if (((intset *)subject->ptr)->intsetLen() > server.set_max_intset_entries)
                    setTypeConvert(subject,OBJ_ENCODING_HT);
                return 1;
            }
        } else {
            /* Failed to get integer from object, convert to regular set. */
            setTypeConvert(subject,OBJ_ENCODING_HT);

            /* The set *was* an intset and this value is not integer
             * encodable, so dictAdd should always work. */
            serverAssert(((dict *)subject->ptr)->dictAdd(sdsdup(value),NULL) == DICT_OK);
            return 1;
        }
    } else {
        serverPanic("Unknown set encoding");
    }
    return 0;
}

int setTypeRemove(robj *setobj, sds value) {
    long long llval;
    if (setobj->encoding == OBJ_ENCODING_HT) {
        if (((dict*)setobj->ptr)->dictDelete(value) == DICT_OK) {
            if (htNeedsResize((dict*)setobj->ptr)) ((dict*)setobj->ptr)->dictResize();
            return 1;
        }
    } else if (setobj->encoding == OBJ_ENCODING_INTSET) {
        if (isSdsRepresentableAsLongLong(value,&llval) == C_OK) {
            int success;
            setobj->ptr = intset::intsetRemove((intset *)setobj->ptr,llval,&success);
            if (success) return 1;
        }
    } else {
        serverPanic("Unknown set encoding");
    }
    return 0;
}

int setTypeIsMember(robj *subject, sds value) {
    long long llval;
    if (subject->encoding == OBJ_ENCODING_HT) {
        return ((dict*)subject->ptr)->dictFind(value) != NULL;
    } else if (subject->encoding == OBJ_ENCODING_INTSET) {
        if (isSdsRepresentableAsLongLong(value,&llval) == C_OK) {
            return ((intset*)subject->ptr)->intsetFind(llval);
        }
    } else {
        serverPanic("Unknown set encoding");
    }
    return 0;
}

setTypeIterator *setTypeInitIterator(robj *subject)
{
    setTypeIterator* si = new (zmalloc(sizeof(setTypeIterator))) setTypeIterator(subject);
    return si;
}

setTypeIterator::setTypeIterator(robj *subject)
        : genericIterator(subject)
, m_dict_iter(NULL)
, m_intset_iter(0)
{
   if (m_encoding == OBJ_ENCODING_HT) {
        m_dict_iter = dictGetIterator((dict*)subject->ptr);
   } else if (m_encoding == OBJ_ENCODING_INTSET) {
        m_intset_iter = 0;
   } else {
        serverPanic("Unknown set encoding");
   }
}

void setTypeReleaseIterator(setTypeIterator *si)
{
    si->~setTypeIterator();
    zfree(si);
}

setTypeIterator::~setTypeIterator()
{
    if (m_encoding == OBJ_ENCODING_HT)
        if (NULL != m_dict_iter)
            dictReleaseIterator(m_dict_iter);
}

/* Move to the next entry in the set. Returns the object at the current
 * position.
 *
 * Since set elements can be internally be stored as SDS strings or
 * simple arrays of integers, setTypeNext returns the encoding of the
 * set object you are iterating, and will populate the appropriate pointer
 * (sdsele) or (llele) accordingly.
 *
 * Note that both the sdsele and llele pointers should be passed and cannot
 * be NULL since the function will try to defensively populate the non
 * used field with values which are easy to trap if misused.
 *
 * When there are no longer elements -1 is returned. */
int setTypeIterator::setTypeNext(sds *sdsele, int64_t *llele)
{
    if (m_encoding == OBJ_ENCODING_HT) {
        dictEntry *de = m_dict_iter->dictNext();
        if (de == NULL)
            return -1;
        *sdsele = (sds)de->dictGetKey();
        *llele = -123456789; /* Not needed. Defensive. */
    } else if (m_encoding == OBJ_ENCODING_INTSET) {
        if (!((intset*)m_subject->ptr)->intsetGet(m_intset_iter++,llele))
            return -1;
        *sdsele = NULL; /* Not needed. Defensive. */
    } else {
        serverPanic("Wrong set encoding in setTypeNext");
    }
    return m_encoding;
}

/* The not copy on write friendly version but easy to use version
 * of setTypeNext() is setTypeNextObject(), returning new SDS
 * strings. So if you don't retain a pointer to this object you should call
 * sdsfree() against it.
 *
 * This function is the way to go for write operations where COW is not
 * an issue. */
sds setTypeIterator::setTypeNextObject()
{
    int64_t int_element;
    sds sds_element;

    int the_encoding = setTypeNext(&sds_element, &int_element);
    switch(the_encoding) {
        case -1:    return NULL;
        case OBJ_ENCODING_INTSET:
            return sdsfromlonglong(int_element);
        case OBJ_ENCODING_HT:
            return sdsdup(sds_element);
        default:
            serverPanic("Unsupported encoding");
    }
    return NULL; /* just to suppress warnings */
}

/* Return random element from a non empty set.
 * The returned element can be a int64_t value if the set is encoded
 * as an "intset" blob of integers, or an SDS string if the set
 * is a regular set.
 *
 * The caller provides both pointers to be populated with the right
 * object. The return value of the function is the object->encoding
 * field of the object and is used by the caller to check if the
 * int64_t pointer or the redis object pointer was populated.
 *
 * Note that both the sdsele and llele pointers should be passed and cannot
 * be NULL since the function will try to defensively populate the non
 * used field with values which are easy to trap if misused. */
int setTypeRandomElement(robj *setobj, sds *sdsele, int64_t *llele) {
    if (setobj->encoding == OBJ_ENCODING_HT) {
        dictEntry *de = ((dict*)setobj->ptr)->dictGetRandomKey();
        *sdsele = (sds)de->dictGetKey();
        *llele = -123456789; /* Not needed. Defensive. */
    } else if (setobj->encoding == OBJ_ENCODING_INTSET) {
        *llele = ((intset *)setobj->ptr)->intsetRandom();
        *sdsele = NULL; /* Not needed. Defensive. */
    } else {
        serverPanic("Unknown set encoding");
    }
    return setobj->encoding;
}

unsigned long setTypeSize(const robj *subject) {
    if (subject->encoding == OBJ_ENCODING_HT) {
        return (((dict*)subject->ptr)->dictSize());
    } else if (subject->encoding == OBJ_ENCODING_INTSET) {
        return ((const intset*)subject->ptr)->intsetLen();
    } else {
        serverPanic("Unknown set encoding");
    }
}

/* Convert the set to specified encoding. The resulting dict (when converting
 * to a hash table) is presized to hold the number of elements in the original
 * set. */
void setTypeConvert(robj *setobj, int enc) {
    ;
    serverAssertWithInfo(NULL,setobj,setobj->type == OBJ_SET &&
                             setobj->encoding == OBJ_ENCODING_INTSET);
    if (enc == OBJ_ENCODING_HT) {
        dict *d = dictCreate(&setDictType, NULL);

        /* Presize the dict to avoid rehashing */

        /* To add the elements we extract integers and create redis objects */
        {
            setTypeIterator si(setobj);
            int64_t int_element;
            sds element;
            while (si.setTypeNext(&element, &int_element) != -1) {
                element = sdsfromlonglong(int_element);
                int ret = d->dictAdd(element, NULL);
                serverAssert(ret == DICT_OK);
            }
        }

        setobj->encoding = OBJ_ENCODING_HT;
        zfree(setobj->ptr);
        setobj->ptr = d;
    } else {
        serverPanic("Unsupported set conversion");
    }
}

void saddCommand(client *c) {
    int j, added = 0;

    robj *_set = lookupKeyWrite(c->m_cur_selected_db,c->m_argv[1]);
    if (_set == NULL) {
        _set = setTypeCreate((sds)c->m_argv[2]->ptr);
        dbAdd(c->m_cur_selected_db,c->m_argv[1],_set);
    } else {
        if (_set->type != OBJ_SET) {
            c->addReply(shared.wrongtypeerr);
            return;
        }
    }

    for (j = 2; j < c->m_argc; j++) {
        if (setTypeAdd(_set,(sds)c->m_argv[j]->ptr)) added++;
    }
    if (added) {
        signalModifiedKey(c->m_cur_selected_db,c->m_argv[1]);
        notifyKeyspaceEvent(NOTIFY_SET,"sadd",c->m_argv[1],c->m_cur_selected_db->m_id);
    }
    server.dirty += added;
    c->addReplyLongLong(added);
}

void sremCommand(client *c) {
    robj *set;
    int j, deleted = 0, keyremoved = 0;

    if ((set = lookupKeyWriteOrReply(c,c->m_argv[1],shared.czero)) == NULL ||
        checkType(c,set,OBJ_SET)) return;

    for (j = 2; j < c->m_argc; j++) {
        if (setTypeRemove(set,(sds)c->m_argv[j]->ptr)) {
            deleted++;
            if (setTypeSize(set) == 0) {
                dbDelete(c->m_cur_selected_db,c->m_argv[1]);
                keyremoved = 1;
                break;
            }
        }
    }
    if (deleted) {
        signalModifiedKey(c->m_cur_selected_db,c->m_argv[1]);
        notifyKeyspaceEvent(NOTIFY_SET,"srem",c->m_argv[1],c->m_cur_selected_db->m_id);
        if (keyremoved)
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->m_argv[1],
                                c->m_cur_selected_db->m_id);
        server.dirty += deleted;
    }
    c->addReplyLongLong(deleted);
}

void smoveCommand(client *c) {
    robj *srcset, *dstset, *ele;
    srcset = lookupKeyWrite(c->m_cur_selected_db,c->m_argv[1]);
    dstset = lookupKeyWrite(c->m_cur_selected_db,c->m_argv[2]);
    ele = c->m_argv[3];

    /* If the source key does not exist return 0 */
    if (srcset == NULL) {
        c->addReply(shared.czero);
        return;
    }

    /* If the source key has the wrong type, or the destination key
     * is set and has the wrong type, return with an error. */
    if (checkType(c,srcset,OBJ_SET) ||
        (dstset && checkType(c,dstset,OBJ_SET))) return;

    /* If srcset and dstset are equal, SMOVE is a no-op */
    if (srcset == dstset) {
        c->addReply(setTypeIsMember(srcset,(sds)ele->ptr) ?
            shared.cone : shared.czero);
        return;
    }

    /* If the element cannot be removed from the src set, return 0. */
    if (!setTypeRemove(srcset,(sds)ele->ptr)) {
        c->addReply(shared.czero);
        return;
    }
    notifyKeyspaceEvent(NOTIFY_SET,"srem",c->m_argv[1],c->m_cur_selected_db->m_id);

    /* Remove the src set from the database when empty */
    if (setTypeSize(srcset) == 0) {
        dbDelete(c->m_cur_selected_db,c->m_argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->m_argv[1],c->m_cur_selected_db->m_id);
    }

    /* Create the destination set when it doesn't exist */
    if (!dstset) {
        dstset = setTypeCreate((sds)ele->ptr);
        dbAdd(c->m_cur_selected_db,c->m_argv[2],dstset);
    }

    signalModifiedKey(c->m_cur_selected_db,c->m_argv[1]);
    signalModifiedKey(c->m_cur_selected_db,c->m_argv[2]);
    server.dirty++;

    /* An extra key has changed when ele was successfully added to dstset */
    if (setTypeAdd(dstset,(sds)ele->ptr)) {
        server.dirty++;
        notifyKeyspaceEvent(NOTIFY_SET,"sadd",c->m_argv[2],c->m_cur_selected_db->m_id);
    }
    c->addReply(shared.cone);
}

void sismemberCommand(client *c) {
    robj *set;

    if ((set = lookupKeyReadOrReply(c,c->m_argv[1],shared.czero)) == NULL ||
        checkType(c,set,OBJ_SET)) return;

    if (setTypeIsMember(set,(sds)c->m_argv[2]->ptr))
        c->addReply(shared.cone);
    else
        c->addReply(shared.czero);
}

void scardCommand(client *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c,c->m_argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_SET)) return;

    c->addReplyLongLong(setTypeSize(o));
}

/* Handle the "SPOP key <count>" variant. The normal version of the
 * command is handled by the spopCommand() function itself. */

/* How many times bigger should be the set compared to the remaining size
 * for us to use the "create new set" strategy? Read later in the
 * implementation for more info. */
#define SPOP_MOVE_STRATEGY_MUL 5

void spopWithCountCommand(client *c) {
    long l;
    unsigned long count, size;
    robj *set;

    /* Get the count argument */
    if (getLongFromObjectOrReply(c,c->m_argv[2],&l,NULL) != C_OK) return;
    if (l >= 0) {
        count = (unsigned long) l;
    } else {
        c->addReply(shared.outofrangeerr);
        return;
    }

    /* Make sure a key with the name inputted exists, and that it's type is
     * indeed a set. Otherwise, return nil */
    if ((set = lookupKeyReadOrReply(c,c->m_argv[1],shared.emptymultibulk))
        == NULL || checkType(c,set,OBJ_SET)) return;

    /* If count is zero, serve an empty multibulk ASAP to avoid special
     * cases later. */
    if (count == 0) {
        c->addReply(shared.emptymultibulk);
        return;
    }

    size = setTypeSize(set);

    /* Generate an SPOP keyspace notification */
    notifyKeyspaceEvent(NOTIFY_SET,"spop",c->m_argv[1],c->m_cur_selected_db->m_id);
    server.dirty += count;

    /* CASE 1:
     * The number of requested elements is greater than or equal to
     * the number of elements inside the set: simply return the whole set. */
    if (count >= size) {
        /* We just return the entire set */
        sunionDiffGenericCommand(c,c->m_argv+1,1,NULL,SET_OP_UNION);

        /* Delete the set as it is now empty */
        dbDelete(c->m_cur_selected_db,c->m_argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->m_argv[1],c->m_cur_selected_db->m_id);

        /* Propagate this command as an DEL operation */
        c->rewriteClientCommandVector(2,shared.del,c->m_argv[1]);
        signalModifiedKey(c->m_cur_selected_db,c->m_argv[1]);
        server.dirty++;
        return;
    }

    /* Case 2 and 3 require to replicate SPOP as a set of SREM commands.
     * Prepare our replication argument vector. Also send the array length
     * which is common to both the code paths. */
    robj *propargv[3];
    propargv[0] = createStringObject("SREM",4);
    propargv[1] = c->m_argv[1];
    c->addReplyMultiBulkLen(count);

    /* Common iteration vars. */
    sds sdsele;
    robj *objele;
    int encoding;
    int64_t llele;
    unsigned long remaining = size-count; /* Elements left after SPOP. */

    /* If we are here, the number of requested elements is less than the
     * number of elements inside the set. Also we are sure that count < size.
     * Use two different strategies.
     *
     * CASE 2: The number of elements to return is small compared to the
     * set size. We can just extract random elements and return them to
     * the set. */
    if (remaining*SPOP_MOVE_STRATEGY_MUL > count) {
        while(count--) {
            /* Emit and remove. */
            encoding = setTypeRandomElement(set,&sdsele,&llele);
            if (encoding == OBJ_ENCODING_INTSET) {
                c->addReplyBulkLongLong(llele);
                objele = createStringObjectFromLongLong(llele);
                set->ptr = intset::intsetRemove((intset *)set->ptr,llele,NULL);
            } else {
                c->addReplyBulkCBuffer(sdsele,sdslen(sdsele));
                objele = createStringObject(sdsele,sdslen(sdsele));
                setTypeRemove(set,sdsele);
            }

            /* Replicate/AOF this command as an SREM operation */
            propargv[2] = objele;
            alsoPropagate(server.sremCommand,c->m_cur_selected_db->m_id,propargv,3,
                PROPAGATE_AOF|PROPAGATE_REPL);
            decrRefCount(objele);
        }
    } else {
        /* CASE 3: The number of elements to return is very big, approaching
         * the size of the set itself. After some time extracting random elements
         * from such a set becomes computationally expensive, so we use
         * a different strategy, we extract random elements that we don't
         * want to return (the elements that will remain part of the set),
         * creating a new set as we do this (that will be stored as the original
         * set). Then we return the elements left in the original set and
         * release it. */
        robj *newset = NULL;

        /* Create a new set with just the remaining elements. */
        while (remaining--) {
            encoding = setTypeRandomElement(set, &sdsele, &llele);
            if (encoding == OBJ_ENCODING_INTSET) {
                sdsele = sdsfromlonglong(llele);
            } else {
                sdsele = sdsdup(sdsele);
            }
            if (!newset) newset = setTypeCreate(sdsele);
            setTypeAdd(newset, sdsele);
            setTypeRemove(set, sdsele);
            sdsfree(sdsele);
        }

        /* Assign the new set as the key value. */
        incrRefCount(set); /* Protect the old set value. */
        dbOverwrite(c->m_cur_selected_db, c->m_argv[1], newset);

        /* Tranfer the old set to the client and release it. */
        {
            setTypeIterator si(set);
            while ((encoding = si.setTypeNext(&sdsele, &llele)) != -1) {
                if (encoding == OBJ_ENCODING_INTSET) {
                    c->addReplyBulkLongLong(llele);
                    objele = createStringObjectFromLongLong(llele);
                } else {
                    c->addReplyBulkCBuffer( sdsele, sdslen(sdsele));
                    objele = createStringObject(sdsele, sdslen(sdsele));
                }

                /* Replicate/AOF this command as an SREM operation */
                propargv[2] = objele;
                alsoPropagate(server.sremCommand, c->m_cur_selected_db->m_id, propargv, 3,
                              PROPAGATE_AOF | PROPAGATE_REPL);
                decrRefCount(objele);
            }
        }
        decrRefCount(set);
    }

    /* Don't propagate the command itself even if we incremented the
     * dirty counter. We don't want to propagate an SPOP command since
     * we propagated the command as a set of SREMs operations using
     * the alsoPropagate() API. */
    decrRefCount(propargv[0]);
    preventCommandPropagation(c);
    signalModifiedKey(c->m_cur_selected_db,c->m_argv[1]);
    server.dirty++;
}

void spopCommand(client *c) {
    robj *set, *ele, *aux;
    sds sdsele;
    int64_t llele;
    int encoding;

    if (c->m_argc == 3) {
        spopWithCountCommand(c);
        return;
    } else if (c->m_argc > 3) {
        c->addReply(shared.syntaxerr);
        return;
    }

    /* Make sure a key with the name inputted exists, and that it's type is
     * indeed a set */
    if ((set = lookupKeyWriteOrReply(c,c->m_argv[1],shared.nullbulk)) == NULL ||
        checkType(c,set,OBJ_SET)) return;

    /* Get a random element from the set */
    encoding = setTypeRandomElement(set,&sdsele,&llele);

    /* Remove the element from the set */
    if (encoding == OBJ_ENCODING_INTSET) {
        ele = createStringObjectFromLongLong(llele);
        set->ptr = intset::intsetRemove((intset *)set->ptr,llele,NULL);
    } else {
        ele = createStringObject(sdsele,sdslen(sdsele));
        setTypeRemove(set,(sds)ele->ptr);
    }

    notifyKeyspaceEvent(NOTIFY_SET,"spop",c->m_argv[1],c->m_cur_selected_db->m_id);

    /* Replicate/AOF this command as an SREM operation */
    aux = createStringObject("SREM",4);
    c->rewriteClientCommandVector(3,aux,c->m_argv[1],ele);
    decrRefCount(aux);

    /* Add the element to the reply */
    c->addReplyBulk(ele);
    decrRefCount(ele);

    /* Delete the set if it's empty */
    if (setTypeSize(set) == 0) {
        dbDelete(c->m_cur_selected_db,c->m_argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->m_argv[1],c->m_cur_selected_db->m_id);
    }

    /* Set has been modified */
    signalModifiedKey(c->m_cur_selected_db,c->m_argv[1]);
    server.dirty++;
}

/* handle the "SRANDMEMBER key <count>" variant. The normal version of the
 * command is handled by the srandmemberCommand() function itself. */

/* How many times bigger should be the set compared to the requested size
 * for us to don't use the "remove elements" strategy? Read later in the
 * implementation for more info. */
#define SRANDMEMBER_SUB_STRATEGY_MUL 3

void srandmemberWithCountCommand(client *c) {
    long l;
    unsigned long count, size;
    int uniq = 1;
    robj *set;
    sds ele;
    int64_t llele;
    int encoding;

    dict *d;

    if (getLongFromObjectOrReply(c,c->m_argv[2],&l,NULL) != C_OK) return;
    if (l >= 0) {
        count = (unsigned long) l;
    } else {
        /* A negative count means: return the same elements multiple times
         * (i.e. don't remove the extracted element after every extraction). */
        count = -l;
        uniq = 0;
    }

    if ((set = lookupKeyReadOrReply(c,c->m_argv[1],shared.emptymultibulk))
        == NULL || checkType(c,set,OBJ_SET)) return;
    size = setTypeSize(set);

    /* If count is zero, serve it ASAP to avoid special cases later. */
    if (count == 0) {
        c->addReply(shared.emptymultibulk);
        return;
    }

    /* CASE 1: The count was negative, so the extraction method is just:
     * "return N random elements" sampling the whole set every time.
     * This case is trivial and can be served without auxiliary data
     * structures. */
    if (!uniq) {
        c->addReplyMultiBulkLen(count);
        while(count--) {
            encoding = setTypeRandomElement(set,&ele,&llele);
            if (encoding == OBJ_ENCODING_INTSET) {
                c->addReplyBulkLongLong(llele);
            } else {
                c->addReplyBulkCBuffer(ele,sdslen(ele));
            }
        }
        return;
    }

    /* CASE 2:
     * The number of requested elements is greater than the number of
     * elements inside the set: simply return the whole set. */
    if (count >= size) {
        sunionDiffGenericCommand(c,c->m_argv+1,1,NULL,SET_OP_UNION);
        return;
    }

    /* For CASE 3 and CASE 4 we need an auxiliary dictionary. */
    d = dictCreate(&objectKeyPointerValueDictType,NULL);

    /* CASE 3:
     * The number of elements inside the set is not greater than
     * SRANDMEMBER_SUB_STRATEGY_MUL times the number of requested elements.
     * In this case we create a set from scratch with all the elements, and
     * subtract random elements to reach the requested number of elements.
     *
     * This is done because if the number of requsted elements is just
     * a bit less than the number of elements in the set, the natural approach
     * used into CASE 3 is highly inefficient. */
    if (count*SRANDMEMBER_SUB_STRATEGY_MUL > size) {
        /* Add all the elements into the temporary dictionary. */
        {
            setTypeIterator si(set);
            while ((encoding = si.setTypeNext(&ele, &llele)) != -1) {
                int retval = DICT_ERR;

                if (encoding == OBJ_ENCODING_INTSET) {
                    retval = d->dictAdd(createStringObjectFromLongLong(llele), NULL);
                } else {
                    retval = d->dictAdd(createStringObject(ele, sdslen(ele)), NULL);
                }
                serverAssert(retval == DICT_OK);
            }
        }
        serverAssert(d->dictSize() == size);

        /* Remove random elements to reach the right count. */
        while(size > count) {
            dictEntry *de;

            de = d->dictGetRandomKey();
            d->dictDelete(de->dictGetKey());
            size--;
        }
    }

    /* CASE 4: We have a big set compared to the requested number of elements.
     * In this case we can simply get random elements from the set and add
     * to the temporary set, trying to eventually get enough unique elements
     * to reach the specified count. */
    else {
        unsigned long added = 0;
        robj *objele;

        while(added < count) {
            encoding = setTypeRandomElement(set,&ele,&llele);
            if (encoding == OBJ_ENCODING_INTSET) {
                objele = createStringObjectFromLongLong(llele);
            } else {
                objele = createStringObject(ele,sdslen(ele));
            }
            /* Try to add the object to the dictionary. If it already exists
             * free it, otherwise increment the number of objects we have
             * in the result dictionary. */
            if (d->dictAdd(objele,NULL) == DICT_OK)
                added++;
            else
                decrRefCount(objele);
        }
    }

    /* CASE 3 & 4: send the result to the user. */
    {
        dictEntry *de;

        c->addReplyMultiBulkLen(count);
        {
            dictIterator di((dict*)d);
            while((de = di.dictNext()) != NULL)
                c->addReplyBulk((robj *)de->dictGetKey());
        }
        dictRelease(d);
    }
}

void srandmemberCommand(client *c) {
    robj *set;
    sds ele;
    int64_t llele;
    int encoding;

    if (c->m_argc == 3) {
        srandmemberWithCountCommand(c);
        return;
    } else if (c->m_argc > 3) {
        c->addReply(shared.syntaxerr);
        return;
    }

    if ((set = lookupKeyReadOrReply(c,c->m_argv[1],shared.nullbulk)) == NULL ||
        checkType(c,set,OBJ_SET)) return;

    encoding = setTypeRandomElement(set,&ele,&llele);
    if (encoding == OBJ_ENCODING_INTSET) {
        c->addReplyBulkLongLong(llele);
    } else {
        c->addReplyBulkCBuffer(ele,sdslen(ele));
    }
}

int qsortCompareSetsByCardinality(const void *s1, const void *s2) {
    if (setTypeSize(*(robj**)s1) > setTypeSize(*(robj**)s2)) return 1;
    if (setTypeSize(*(robj**)s1) < setTypeSize(*(robj**)s2)) return -1;
    return 0;
}

/* This is used by SDIFF and in this case we can receive NULL that should
 * be handled as empty sets. */
int qsortCompareSetsByRevCardinality(const void *s1, const void *s2) {
    robj *o1 = *(robj**)s1, *o2 = *(robj**)s2;
    unsigned long first = o1 ? setTypeSize(o1) : 0;
    unsigned long second = o2 ? setTypeSize(o2) : 0;

    if (first < second) return 1;
    if (first > second) return -1;
    return 0;
}

void sinterGenericCommand(client *c, robj **setkeys,
                          unsigned long setnum, robj *dstkey) {
    robj **sets = (robj **) zmalloc(sizeof(robj *) * setnum);
    robj *dstset = NULL;
    sds elesds;
    int64_t intobj;
    void *replylen = NULL;
    unsigned long j, cardinality = 0;
    int encoding;

    for (j = 0; j < setnum; j++) {
        robj *setobj = dstkey ?
                       lookupKeyWrite(c->m_cur_selected_db, setkeys[j]) :
                       lookupKeyRead(c->m_cur_selected_db, setkeys[j]);
        if (!setobj) {
            zfree(sets);
            if (dstkey) {
                if (dbDelete(c->m_cur_selected_db, dstkey)) {
                    signalModifiedKey(c->m_cur_selected_db, dstkey);
                    server.dirty++;
                }
                c->addReply( shared.czero);
            } else {
                c->addReply( shared.emptymultibulk);
            }
            return;
        }
        if (checkType(c, setobj, OBJ_SET)) {
            zfree(sets);
            return;
        }
        sets[j] = setobj;
    }
    /* Sort sets from the smallest to largest, this will improve our
     * algorithm's performance */
    qsort(sets, setnum, sizeof(robj *), qsortCompareSetsByCardinality);

    /* The first thing we should output is the total number of elements...
     * since this is a multi-bulk write, but at this stage we don't know
     * the intersection set size, so we use a trick, append an empty object
     * to the output list and save the pointer to later modify it with the
     * right length */
    if (!dstkey) {
        replylen = c->addDeferredMultiBulkLength();
    } else {
        /* If we have a target key where to store the resulting set
         * create this key with an empty set inside */
        dstset = createIntsetObject();
    }

    /* Iterate all the elements of the first (smallest) set, and test
     * the element against all the other sets, if at least one set does
     * not include the element it is discarded */
    {
        setTypeIterator si(sets[0]);
        while ((encoding = si.setTypeNext(&elesds, &intobj)) != -1) {
            for (j = 1; j < setnum; j++) {
                if (sets[j] == sets[0]) continue;
                if (encoding == OBJ_ENCODING_INTSET) {
                    /* intset with intset is simple... and fast */
                    if (sets[j]->encoding == OBJ_ENCODING_INTSET &&
                        !((intset *) sets[j]->ptr)->intsetFind(intobj)) {
                        break;
                        /* in order to compare an integer with an object we
                         * have to use the generic function, creating an object
                         * for this */
                    } else if (sets[j]->encoding == OBJ_ENCODING_HT) {
                        elesds = sdsfromlonglong(intobj);
                        if (!setTypeIsMember(sets[j], elesds)) {
                            sdsfree(elesds);
                            break;
                        }
                        sdsfree(elesds);
                    }
                } else if (encoding == OBJ_ENCODING_HT) {
                    if (!setTypeIsMember(sets[j], elesds)) {
                        break;
                    }
                }
            }

            /* Only take action when all sets contain the member */
            if (j == setnum) {
                if (!dstkey) {
                    if (encoding == OBJ_ENCODING_HT)
                        c->addReplyBulkCBuffer( elesds, sdslen(elesds));
                    else
                        c->addReplyBulkLongLong(intobj);
                    cardinality++;
                } else {
                    if (encoding == OBJ_ENCODING_INTSET) {
                        elesds = sdsfromlonglong(intobj);
                        setTypeAdd(dstset, elesds);
                        sdsfree(elesds);
                    } else {
                        setTypeAdd(dstset, elesds);
                    }
                }
            }
        }
    }

    if (dstkey) {
        /* Store the resulting set into the target, if the intersection
         * is not an empty set. */
        int deleted = dbDelete(c->m_cur_selected_db,dstkey);
        if (setTypeSize(dstset) > 0) {
            dbAdd(c->m_cur_selected_db,dstkey,dstset);
            c->addReplyLongLong(setTypeSize(dstset));
            notifyKeyspaceEvent(NOTIFY_SET,"sinterstore",
                dstkey,c->m_cur_selected_db->m_id);
        } else {
            decrRefCount(dstset);
            c->addReply(shared.czero);
            if (deleted)
                notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                    dstkey,c->m_cur_selected_db->m_id);
        }
        signalModifiedKey(c->m_cur_selected_db,dstkey);
        server.dirty++;
    } else {
        c->setDeferredMultiBulkLength(replylen,cardinality);
    }
    zfree(sets);
}

void sinterCommand(client *c) {
    sinterGenericCommand(c,c->m_argv+1,c->m_argc-1,NULL);
}

void sinterstoreCommand(client *c) {
    sinterGenericCommand(c,c->m_argv+2,c->m_argc-2,c->m_argv[1]);
}

#define SET_OP_UNION 0
#define SET_OP_DIFF 1
#define SET_OP_INTER 2

void sunionDiffGenericCommand(client *c, robj **setkeys, int setnum,
                              robj *dstkey, int op) {
    robj **sets = (robj **)zmalloc(sizeof(robj*)*setnum);
    robj *dstset = NULL;
    sds ele;
    int j, cardinality = 0;
    int diff_algo = 1;

    for (j = 0; j < setnum; j++) {
        robj *setobj = dstkey ?
            lookupKeyWrite(c->m_cur_selected_db,setkeys[j]) :
            lookupKeyRead(c->m_cur_selected_db,setkeys[j]);
        if (!setobj) {
            sets[j] = NULL;
            continue;
        }
        if (checkType(c,setobj,OBJ_SET)) {
            zfree(sets);
            return;
        }
        sets[j] = setobj;
    }

    /* Select what DIFF algorithm to use.
     *
     * Algorithm 1 is O(N*M) where N is the size of the element first set
     * and M the total number of sets.
     *
     * Algorithm 2 is O(N) where N is the total number of elements in all
     * the sets.
     *
     * We compute what is the best bet with the current input here. */
    if (op == SET_OP_DIFF && sets[0]) {
        long long algo_one_work = 0, algo_two_work = 0;

        for (j = 0; j < setnum; j++) {
            if (sets[j] == NULL) continue;

            algo_one_work += setTypeSize(sets[0]);
            algo_two_work += setTypeSize(sets[j]);
        }

        /* Algorithm 1 has better constant times and performs less operations
         * if there are elements in common. Give it some advantage. */
        algo_one_work /= 2;
        diff_algo = (algo_one_work <= algo_two_work) ? 1 : 2;

        if (diff_algo == 1 && setnum > 1) {
            /* With algorithm 1 it is better to order the sets to subtract
             * by decreasing size, so that we are more likely to find
             * duplicated elements ASAP. */
            qsort(sets+1,setnum-1,sizeof(robj*),
                qsortCompareSetsByRevCardinality);
        }
    }

    /* We need a temp set object to store our union. If the dstkey
     * is not NULL (that is, we are inside an SUNIONSTORE operation) then
     * this set object will be the resulting object to set into the target key*/
    dstset = createIntsetObject();

    if (op == SET_OP_UNION) {
        /* Union is trivial, just add every element of every set to the
         * temporary set. */
        for (j = 0; j < setnum; j++) {
            if (!sets[j]) continue; /* non existing keys are like empty sets */

            setTypeIterator si(sets[j]);
            while((ele = si.setTypeNextObject()) != NULL) {
                if (setTypeAdd(dstset,ele)) cardinality++;
                sdsfree(ele);
            }
        }
    } else if (op == SET_OP_DIFF && sets[0] && diff_algo == 1) {
        /* DIFF Algorithm 1:
         *
         * We perform the diff by iterating all the elements of the first set,
         * and only adding it to the target set if the element does not exist
         * into all the other sets.
         *
         * This way we perform at max N*M operations, where N is the size of
         * the first set, and M the number of sets. */
        setTypeIterator si(sets[0]);
        while((ele = si.setTypeNextObject()) != NULL) {
            for (j = 1; j < setnum; j++) {
                if (!sets[j]) continue; /* no key is an empty set. */
                if (sets[j] == sets[0]) break; /* same set! */
                if (setTypeIsMember(sets[j],ele)) break;
            }
            if (j == setnum) {
                /* There is no other set with this element. Add it. */
                setTypeAdd(dstset,ele);
                cardinality++;
            }
            sdsfree(ele);
        }

    } else if (op == SET_OP_DIFF && sets[0] && diff_algo == 2) {
        /* DIFF Algorithm 2:
         *
         * Add all the elements of the first set to the auxiliary set.
         * Then remove all the elements of all the next sets from it.
         *
         * This is O(N) where N is the sum of all the elements in every
         * set. */
        for (j = 0; j < setnum; j++) {
            if (!sets[j]) continue; /* non existing keys are like empty sets */

            setTypeIterator si(sets[j]);
            while((ele = si.setTypeNextObject()) != NULL) {
                if (j == 0) {
                    if (setTypeAdd(dstset,ele)) cardinality++;
                } else {
                    if (setTypeRemove(dstset,ele)) cardinality--;
                }
                sdsfree(ele);
            }

            /* Exit if result set is empty as any additional removal
             * of elements will have no effect. */
            if (cardinality == 0)
                break;
        }
    }

    /* Output the content of the resulting set, if not in STORE mode */
    if (!dstkey) {
        c->addReplyMultiBulkLen( cardinality);
        {
            setTypeIterator si(dstset);
            while ((ele = si.setTypeNextObject()) != NULL) {
                c->addReplyBulkCBuffer( ele, sdslen(ele));
                sdsfree(ele);
            }
        }   // the braces make sure si is destructed before decrRefCount might dispose of
            // dstset. si might contain a dictIterator that points to dstset
        decrRefCount(dstset);
    } else {
        /* If we have a target key where to store the resulting set
         * create this key with the result set inside */
        int deleted = dbDelete(c->m_cur_selected_db,dstkey);
        if (setTypeSize(dstset) > 0) {
            dbAdd(c->m_cur_selected_db,dstkey,dstset);
            c->addReplyLongLong(setTypeSize(dstset));
            notifyKeyspaceEvent(NOTIFY_SET,
                op == SET_OP_UNION ? "sunionstore" : "sdiffstore",
                dstkey,c->m_cur_selected_db->m_id);
        } else {
            decrRefCount(dstset);
            c->addReply(shared.czero);
            if (deleted)
                notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                    dstkey,c->m_cur_selected_db->m_id);
        }
        signalModifiedKey(c->m_cur_selected_db,dstkey);
        server.dirty++;
    }
    zfree(sets);
}

void sunionCommand(client *c) {
    sunionDiffGenericCommand(c,c->m_argv+1,c->m_argc-1,NULL,SET_OP_UNION);
}

void sunionstoreCommand(client *c) {
    sunionDiffGenericCommand(c,c->m_argv+2,c->m_argc-2,c->m_argv[1],SET_OP_UNION);
}

void sdiffCommand(client *c) {
    sunionDiffGenericCommand(c,c->m_argv+1,c->m_argc-1,NULL,SET_OP_DIFF);
}

void sdiffstoreCommand(client *c) {
    sunionDiffGenericCommand(c,c->m_argv+2,c->m_argc-2,c->m_argv[1],SET_OP_DIFF);
}

void sscanCommand(client *c) {
    robj *set;
    unsigned long cursor;

    if (parseScanCursorOrReply(c,c->m_argv[2],&cursor) == C_ERR) return;
    if ((set = lookupKeyReadOrReply(c,c->m_argv[1],shared.emptyscan)) == NULL ||
        checkType(c,set,OBJ_SET)) return;
    scanGenericCommand(c,set,cursor);
}
