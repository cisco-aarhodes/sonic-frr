#include "command_match.h"
#include "command_parse.h"
#include <zebra.h>
#include "memory.h"

/* matcher helper prototypes */
static int
add_nexthops(struct list *, struct graph_node *);

static struct list *
match_command_r (struct graph_node *, vector, unsigned int);

static int
score_precedence (struct graph_node *);

static enum match_type
min_match_level(enum node_type type);

/* token matcher prototypes */
static enum match_type
match_ipv4 (const char *);

static enum match_type
match_ipv4_prefix (const char *);

static enum match_type
match_ipv6 (const char *);

static enum match_type
match_ipv6_prefix (const char *);

static enum match_type
match_range (struct graph_node *, const char *str);

static enum match_type
match_word (struct graph_node *, const char *);

static enum match_type
match_number (struct graph_node *, const char *);

static enum match_type
match_variable (struct graph_node *, const char *);

static enum match_type
match_token (struct graph_node *, char *);

/* matching functions */

static struct graph_node *
copy_node (struct graph_node *node)
{
  struct graph_node *new = new_node(node->type);
  new->children = NULL;
  new->is_start = node->is_start;
  new->end      = NULL;
  new->text     = node->text ? XSTRDUP(MTYPE_CMD_TOKENS, node->text) : NULL;
  new->value    = node->value;
  new->min      = node->min;
  new->max      = node->max;
  new->element  = node->element ? copy_cmd_element(node->element) : NULL;
  new->arg      = node->arg ? XSTRDUP(MTYPE_CMD_TOKENS, node->arg) : NULL;
  new->refs     = 0;
  return new;
}


/* Linked list data deletion callback */
static void
delete_nodelist (void *node) {
  free_node ((struct graph_node *) node);
}

struct cmd_element *
match_command (struct graph_node *start, const char *line, struct list **argv)
{
  // parse command
  vector vline = cmd_make_strvec (line);

  for (unsigned int i = 0; i < vector_active(start->children); i++)
  {
    // call recursive builder on each starting child
    *argv = match_command_r(vector_slot(start->children, i), vline, 0);
    // if any of them succeed, return their argv
    // since all command DFA's must begin with a word, there can only be
    // one valid return value
    if (*argv) break;
  }

  // walk the list, find the END_GN, return that
  if (*argv) {
    struct listnode *ln;
    struct graph_node *gn;
    char buf[50];
    for (ALL_LIST_ELEMENTS_RO(*argv,ln,gn)) {
      describe_node(gn, buf, 50);
      fprintf(stderr, "%s[%d]\n", buf, gn->type);
      if (gn->type == END_GN)
        return gn->element;
    }
    assert(0);
  }

  return NULL;
}

/**
 * Matches a given input line against a DFA.
 *
 * Builds an argument list given a DFA and a matching input line. This function
 * should be passed the start node of the DFA, a matching input line, and the
 * index of the first token in the input line.
 *
 * First the function determines if the node it is passed matches the first
 * token of input. If it does not, it returns NULL. If it does match, then it
 * saves the input token as the head of an argument list.
 *
 * The next step is to see if there is further input in the input line. If
 * there is not, the current node's children are searched to see if any of them
 * are leaves (type END_GN). If this is the case, then the bottom of the
 * recursion stack has been reached, and the argument list (with one node) is
 * returned. If it is not the case, NULL is returned, indicating that there is
 * no match for the input along this path.
 *
 * If there is further input, then the function recurses on each of the current
 * node's children, passing them the input line minus the token that was just
 * matched. For each child, the return value of the recursive call is
 * inspected. If it is null, then there is no match for the input along the
 * subgraph headed by that child. If it is not null, then there is at least one
 * input match in that subgraph (more on this in a moment).
 *
 * If a recursive call on a child returns a non-null value, then it has matched
 * the input given it on the subgraph that starts with that child. However, due
 * to the flexibility of the grammar, it is sometimes the case that two or more
 * child graphs match the same input (two or more of the recursive calls have
 * non-NULL return values). This is not a valid state, since only one true
 * match is possible. In order to resolve this conflict, the function keeps a
 * reference to the child node that most specifically matches the input. This
 * is done by assigning each node type a precedence. If a child is found to
 * match the remaining input, then the precedence values of the current
 * best-matching child and this new match are compared. The node with higher
 * precedence is kept, and the other match is discarded. Due to the recursive
 * nature of this function, it is only necessary to compare the precedence of
 * immediate children, since all subsequent children will already have been
 * disambiguated in this way.
 *
 * In the event that two children are found to match with the same precedence,
 * then the input is ambiguous for the passed cmd_element and NULL is returned.
 *
 * The ultimate return value is an ordered linked list of nodes that comprise
 * the best match for the command, each with their `arg` fields pointing to the
 * matching token string.
 *
 * @param[out] start the start node.
 * @param[in] vline the vectorized input line.
 * @param[in] n the index of the first input token. Should be 0 for external
 * callers.
 */
static struct list *
match_command_r (struct graph_node *start, vector vline, unsigned int n)
{
  // get the minimum match level that can count as a full match
  enum match_type minmatch = min_match_level(start->type);

  // get the current operating token
  char *token = vector_slot(vline, n);

  // if we don't match this node, die
  if (match_token(start, token) < minmatch)
    return NULL;

  // pointers for iterating linklist
  struct graph_node *gn;
  struct listnode   *ln;

  // get all possible nexthops
  struct list *next = list_new();
  add_nexthops(next, start);

  // if we're at the end of input, need END_GN or no match
  if (n+1 == vector_active (vline)) {
    for (ALL_LIST_ELEMENTS_RO(next,ln,gn)) {
      if (gn->type == END_GN) {
        // we have a match, so build an argv and pass it back up the chain
        struct list *argv = list_new();
        argv->del = &delete_nodelist;
        struct graph_node *curr = copy_node(start);
        curr->arg = XSTRDUP(MTYPE_CMD_TOKENS, token);
        // add the current node
        listnode_add(argv, curr);
        // add the end node
        listnode_add(argv, copy_node(gn));
        // clean up
        list_delete (next);
        return argv;
      }
    }
    // no END_GN found, free resources and return null
    list_delete (next);
    return NULL;
  }

  // otherwise recurse on all nexthops
  struct list *bestmatch = NULL;
  for (ALL_LIST_ELEMENTS_RO(next,ln,gn))
    {
      if (gn->type == END_GN) // skip END_GN since we aren't at end of input
        continue;

      // get the result of the next node
      struct list *result = match_command_r (gn, vline, n+1);

      // compare to our current best match, and save if it's better
      if (result) {
        if (bestmatch) {
          int currprec = score_precedence (listgetdata(listhead(bestmatch)));
          int rsltprec = score_precedence (gn);
          if (currprec < rsltprec)
            list_delete (result);
          if (currprec > rsltprec) {
            list_delete (bestmatch);
            bestmatch = result;
          }
          if (currprec == rsltprec) {
            list_delete (bestmatch);
            list_delete (result);
            list_delete (next);
            return NULL;
          }
        }
        else
          bestmatch = result;
      }
    }

  if (bestmatch) {
    // if we have a best match, prepend this node + matching token
    // to argv and pass it up the chain
    struct graph_node *curr = copy_node(start);
    curr->arg = XSTRDUP(MTYPE_CMD_TOKENS, token);
    list_add_node_prev (bestmatch, bestmatch->head, curr);
    // cleanup
    list_delete (next);
    return bestmatch;
  }
  else
    return NULL;
}

struct list *
match_command_complete (struct graph_node *start, const char *line)
{
  // vectorize command line
  vector vline = cmd_make_strvec (line);

  // pointer to next input token to match
  char *token;

  struct list *current  = list_new(), // current nodes to match input token against
              *next     = list_new(); // possible next hops to current input token

  // pointers used for iterating lists
  struct graph_node *gn;
  struct listnode *node;

  // add all children of start node to list
  add_nexthops(next, start);

  unsigned int idx;
  for (idx = 0; idx < vector_active(vline) && next->count > 0; idx++)
  {
    list_free (current);
    current = next;
    next = list_new();

    token = vector_slot(vline, idx);

    for (ALL_LIST_ELEMENTS_RO(current,node,gn))
    {
      switch (match_token(gn, token)) {
        case partly_match: // if it's a partial match, add this node to possible next hops
          listnode_add(next, gn);
          break;
        case exact_match: // if it's an exact match, add this node's children to next hops
          add_nexthops(next, gn);
          break;
        default: // otherwise do nothing
          break;
      }
    }
  }

  /* Variable summary
   * -----------------------------------------------------------------
   * token    = last input token processed
   * idx      = index in `command` of last token processed
   * current  = set of all transitions from the previous input token
   * matched  = set of all nodes reachable with current input
   * next     = set of all nodes reachable from all nodes in `matched`
   */
  list_free (current);

  cmd_free_strvec(vline);

  return next;
}

/**
 * Adds all children that are reachable by one parser hop
 * to the given list. NUL_GN, SELECTOR_GN, and OPTION_GN
 * nodes are treated as transparent.
 *
 * @param[out] l the list to add the children to
 * @param[in] node the node to get the children of
 * @return the number of children added to the list
 */
static int
add_nexthops(struct list *l, struct graph_node *node)
{
  int added = 0;
  struct graph_node *child;
  for (unsigned int i = 0; i < vector_active(node->children); i++)
  {
    child = vector_slot(node->children, i);
    switch (child->type) {
      case OPTION_GN:
      case SELECTOR_GN:
      case NUL_GN:
        added += add_nexthops(l, child);
        break;
      default:
        listnode_add(l, child);
        added++;
    }
  }
  return added;
}


/* matching utility functions */

/**
 * Determines the minimum acceptable matching level
 * for a given node type that can be accepted as a
 * full match. Used for things like abbreviating
 * commands, e.g. `conf t`.
 */
static enum match_type
min_match_level(enum node_type type)
{
  switch (type) {
    case WORD_GN:
      return partly_match;
    default:
      return exact_match;
  }
}

static int
score_precedence (struct graph_node *node)
{
  switch (node->type)
  {
    // these should be mutually exclusive,
    // or never compared
    case IPV4_GN:
    case IPV4_PREFIX_GN:
    case IPV6_GN:
    case IPV6_PREFIX_GN:
    case RANGE_GN:
    case NUMBER_GN:
      return 1;
    case WORD_GN:
      return 2;
    case VARIABLE_GN:
      return 3;
    default:
      return 10;
  }
}

static enum match_type
match_token (struct graph_node *node, char *token)
{
  switch (node->type) {
    case WORD_GN:
      return match_word (node, token);
    case IPV4_GN:
      return match_ipv4 (token);
    case IPV4_PREFIX_GN:
      return match_ipv4_prefix (token);
    case IPV6_GN:
      return match_ipv6 (token);
    case IPV6_PREFIX_GN:
      return match_ipv6_prefix (token);
    case RANGE_GN:
      return match_range (node, token);
    case NUMBER_GN:
      return match_number (node, token);
    case VARIABLE_GN:
      return match_variable (node, token);
    case END_GN:
    default:
      return no_match;
  }
}

#define IPV4_ADDR_STR   "0123456789."
#define IPV4_PREFIX_STR "0123456789./"

static enum match_type
match_ipv4 (const char *str)
{
  const char *sp;
  int dots = 0, nums = 0;
  char buf[4];

  if (str == NULL)
    return partly_match;

  for (;;)
    {
      memset (buf, 0, sizeof (buf));
      sp = str;
      while (*str != '\0')
        {
          if (*str == '.')
            {
              if (dots >= 3)
                return no_match;

              if (*(str + 1) == '.')
                return no_match;

              if (*(str + 1) == '\0')
                return partly_match;

              dots++;
              break;
            }
          if (!isdigit ((int) *str))
            return no_match;

          str++;
        }

      if (str - sp > 3)
        return no_match;

      strncpy (buf, sp, str - sp);
      if (atoi (buf) > 255)
        return no_match;

      nums++;

      if (*str == '\0')
        break;

      str++;
    }

  if (nums < 4)
    return partly_match;

  return exact_match;
}

static enum match_type
match_ipv4_prefix (const char *str)
{
  const char *sp;
  int dots = 0;
  char buf[4];

  if (str == NULL)
    return partly_match;

  for (;;)
    {
      memset (buf, 0, sizeof (buf));
      sp = str;
      while (*str != '\0' && *str != '/')
        {
          if (*str == '.')
            {
              if (dots == 3)
                return no_match;

              if (*(str + 1) == '.' || *(str + 1) == '/')
                return no_match;

              if (*(str + 1) == '\0')
                return partly_match;

              dots++;
              break;
            }

          if (!isdigit ((int) *str))
            return no_match;

          str++;
        }

      if (str - sp > 3)
        return no_match;

      strncpy (buf, sp, str - sp);
      if (atoi (buf) > 255)
        return no_match;

      if (dots == 3)
        {
          if (*str == '/')
            {
              if (*(str + 1) == '\0')
                return partly_match;

              str++;
              break;
            }
          else if (*str == '\0')
            return partly_match;
        }

      if (*str == '\0')
        return partly_match;

      str++;
    }

  sp = str;
  while (*str != '\0')
    {
      if (!isdigit ((int) *str))
        return no_match;

      str++;
    }

  if (atoi (sp) > 32)
    return no_match;

  return exact_match;
}

#ifdef HAVE_IPV6
#define IPV6_ADDR_STR   "0123456789abcdefABCDEF:."
#define IPV6_PREFIX_STR "0123456789abcdefABCDEF:./"

static enum match_type
match_ipv6 (const char *str)
{
  struct sockaddr_in6 sin6_dummy;
  int ret;

  if (str == NULL)
    return partly_match;

  if (strspn (str, IPV6_ADDR_STR) != strlen (str))
    return no_match;

  ret = inet_pton(AF_INET6, str, &sin6_dummy.sin6_addr);

  if (ret == 1)
    return exact_match;

  return no_match;
}

static enum match_type
match_ipv6_prefix (const char *str)
{
  struct sockaddr_in6 sin6_dummy;
  const char *delim = "/\0";
  char *dupe, *prefix, *mask, *context, *endptr;
  int nmask = -1;

  if (str == NULL)
    return partly_match;

  if (strspn (str, IPV6_PREFIX_STR) != strlen (str))
    return no_match;

  /* tokenize to address + mask */
  dupe = XMALLOC(MTYPE_TMP, strlen(str)+1);
  strncpy(dupe, str, strlen(str)+1);
  prefix = strtok_r(dupe, delim, &context);
  mask   = strtok_r(NULL, delim, &context);

  if (!mask)
    return partly_match;

  /* validate prefix */
  if (inet_pton(AF_INET6, prefix, &sin6_dummy.sin6_addr) != 1)
    return no_match;

  /* validate mask */
  nmask = strtol (mask, &endptr, 10);
  if (*endptr != '\0' || nmask < 0 || nmask > 128)
    return no_match;

  XFREE(MTYPE_TMP, dupe);

  return exact_match;
}
#endif

static enum match_type
match_range (struct graph_node *rangenode, const char *str)
{
  char *endptr = NULL;
  signed long long val;

  if (str == NULL)
    return 1;

  val = strtoll (str, &endptr, 10);
  if (*endptr != '\0')
    return 0;
  val = llabs(val);

  if (val < rangenode->min || val > rangenode->max)
    return no_match;
  else
    return exact_match;
}

static enum match_type
match_word(struct graph_node *wordnode, const char *word)
{
  // if the passed token is null or 0 length, partly match
  if (!word || !strlen(word))
    return partly_match;

  // if the passed token is strictly a prefix of the full word, partly match
  if (strlen(word) < strlen(wordnode->text))
    return !strncmp(wordnode->text, word, strlen(word)) ? partly_match : no_match;

  // if they are the same length and exactly equal, exact match
  else if (strlen(word) == strlen(wordnode->text))
    return !strncmp(wordnode->text, word, strlen(word)) ? exact_match : no_match;

  return no_match;
}

static enum match_type
match_number(struct graph_node *numnode, const char *word)
{
  if (!strcmp("\0", word)) return no_match;
  char *endptr;
  long num = strtol(word, &endptr, 10);
  if (endptr != '\0') return no_match;
  return num == numnode->value ? exact_match : no_match;
}

#define VARIABLE_ALPHABET "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz1234567890:"

static enum match_type
match_variable(struct graph_node *varnode, const char *word)
{
  return strlen(word) == strspn(word, VARIABLE_ALPHABET) ?
     exact_match : no_match;
}
