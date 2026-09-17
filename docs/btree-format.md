# B+tree format

Code: `engine/src/engine/btree.cpp`, API `engine/include/archivum/engine/btree.h`.
Every b-tree page is an ordinary pager page (`docs/page-format.md`): the
last 4 bytes are the CRC32C trailer written by the pager, and the tree
uses the rest. Integers are little-endian. Page numbers are 64-bit.

## Node header (24 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | u8 | type: 1 leaf, 2 interior |
| 1 | u8 | reserved, 0 |
| 2 | u16 | cell count |
| 4 | u16 | content start: cells grow down from the page end to here |
| 6 | u16 | fragmented bytes (freed cell space not yet reclaimed) |
| 8 | u64 | interior: rightmost child; leaf: next leaf (0 at the end) |
| 16 | u64 | leaf: previous leaf (0 at the start); interior: 0 |
| 24 | u16 × count | slot array: cell offsets in key order |

Cells are appended at the content start; a deletion adds the cell's size
to the fragmented count. When a cell does not fit but the free space plus
the fragmented bytes would hold it, the node is compacted in place.

## Cells

Leaf cell: `u16 key length | u8 flags | u32 value length | key | payload`.
With flag bit 0 clear the payload is the value inline. With it set the
payload is a `u64` first overflow page and the value is the concatenation
of the overflow chain, `value length` bytes long.

Interior cell: `u16 key length | u64 left child | key`. A child under
separator K holds keys strictly less than K; the rightmost child (in the
header) holds keys greater than or equal to the last separator.

Overflow page: `u64 next page | data` filling the usable page bytes; 0
ends the chain.

Keys compare as unsigned bytes. The typed layer encodes keys so that this
order is the type order (`docs/store-format.md`).

## Limits

With `usable = page size − 4` and `max cell = (usable − 24) / 4 − 2`, at
least four cells fit on any node. A key may be at most `max cell − 15`
bytes (the leaf cell header plus an overflow pointer); a value of any
length is accepted, spilled to overflow once the cell would exceed the
maximum. For a 4096-byte page the key limit is 1002 bytes; for the
1024-byte pages the tests use, 232.

## Operations

- **Root is fixed.** `create` allocates one empty leaf and its page number
  is the tree's identity for ever. When the root splits, its left half is
  copied to a new page and the root page is rewritten as an interior node
  with one separator; the right half goes to a second new page.
- **Split** divides cells by cumulative size so both halves are about
  half full; the separator promoted from a leaf split is the first key of
  the right half, and from an interior split the middle key moves up.
- **Erase** removes the cell. A leaf left empty is unlinked from its
  neighbours and freed; its parent drops the child, and an interior node
  left with only its rightmost child collapses: the parent's pointer is
  redirected to that child and the node is freed. If the root is interior
  and has only its rightmost child, that child's contents are copied into
  the root page and the child is freed. Nodes are otherwise not merged;
  their space is reused by later inserts.
- **Cursor** walks leaves through the next and previous links; `seek`
  positions at the first key ≥ the argument.

## Checker

`BTree::check` walks every node and verifies: node types by depth (every
leaf at the same depth), cell offsets inside the page and not overlapping
the slot array, keys strictly increasing within a node, every key inside
the bounds its ancestors' separators define, leaf `prev` links matching
the walk order, overflow chains of the declared length, and no page owned
twice. It returns the set of pages the tree owns so that the store can
cross-check ownership against the free list and the other trees.
