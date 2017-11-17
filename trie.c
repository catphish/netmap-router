/*
 * FIT VUT - PDS PROJECT 2012/2013 - LONGEST PREFIX MATCH
 * (c) Ondrej Fibich <xfibic01@stud.fit.vutbr.cz>
 */

#include <stdio.h>
#include <arpa/inet.h>

#include "trie.h"

/*
 * Tree for storing prefixes and searching for longest match on stored prefixes.
 */

/**
 * Allocates memory for a node and initializes it
 *
 * @param t tree
 * @return node
 */
static node_t *_node_malloc(trie_t *t)
{
    node_t *n;

    // add new memory cell
    if (t->last == NULL || t->last->ln + 1 == TRIE_MEMORY_CELL_SIZE)
    {
        trie_memory_t *pom = t->last;
        t->last = (trie_memory_t *) malloc(sizeof (trie_memory_t));

        if (t->last == NULL)
        {
            fprintf(stderr, "Cannot allocate memory\n");
            exit(EXIT_FAILURE);
        }

        t->last->ln = 0;
        t->last->next = NULL;

        if (pom != NULL)
        {
            pom->next = t->last;
        }
    }
    // get node
    n = &(t->last->cells[t->last->ln]);
    t->last->ln++; // next cell
    n->next_hop_ip = 0;
    n->next_hop_interface = 0;
    n->l = NULL;
    n->r = NULL;
    // return
    return n;
}

/**
 * Initializes the trie tree
 *
 * @param t
 */
void trie_init(trie_t *t)
{
    t->first = t->last = NULL;
    t->root = _node_malloc(t);
    t->first = t->last;
}

/**
 * Destroys allocated memory
 *
 * @param t
 */
void trie_destroy(trie_t *t)
{
    trie_memory_t *current = t->first, *pom;

    while (current != NULL)
    {
        pom = current->next;
        free(current);
        current = pom;
    }
}

/**
 * Puts the given network address to prefix tree
 *
 * @param t tree
 * @param ip IP of subnet
 * @param cidr subnet length
 * @param next_hop_ip the next hop address
 * @param next_hop_interface the interface of the next hop
 */
void trie_node_put(trie_t *t, uint8_t *ip, uint8_t cidr, uint32_t next_hop_ip, uint8_t next_hop_interface)
{
    node_t *current = t->root;
    int i;

    // add each bit of address prefix to tree
    for (i = 0; i < cidr; i++)
    {
        // bit is 1 => go right
        if (((ip[i / 8] >> (7 - i % 8)) & 1) > 0)
        {
            // create path if not exists
            if (current->r == NULL)
            {
                current->r = _node_malloc(t);
            }
            // next
            current = current->r;
        }
        // bit is 0 => go left
        else
        {
            // create path if not exists
            if (current->l == NULL)
            {
                current->l = _node_malloc(t);
            }
            // next
            current = current->l;
        }
        // last bit => add to node
        if ((i + 1) == cidr)
        {
            current->next_hop_ip = next_hop_ip;
            current->next_hop_interface = next_hop_interface;
        }
    }
}

/**
 * Searches for network for the given IP address
 *
 * @param t trie tree
 * @param ip IP of subnet
 * @param ln length of IP (4/16)
 * @param location to store next hop ip
 * @param location to store next hop interface
 * @return if success than >0 else 0
 */
uint8_t trie_node_search(trie_t *t, uint8_t *ip, uint32_t *next_hop_ip, uint8_t *next_hop_interface)
{
    node_t *current = t->root;
    uint8_t found = 0;
    uint8_t byte;
    int i = 0;
    int j;

    while(1) {
        byte = ip[i++];
        for (j=0; j<8; j++)
        {
            // search right
            if (byte & 0x80)
            {
                current = current->r;
            }
            // search left
            else
            {
                current = current->l;
            }
            // end of road
            if (current == NULL)
            {
                return found;
            }
            // change last network
            if (current->next_hop_interface)
            {
                *next_hop_interface = current->next_hop_interface;
                *next_hop_ip = current->next_hop_ip;
                found = 1;
            }
            byte <<= 1;
        }
    }

    return found;
}
