#ifndef MIME_MAGIC_PRIV_HH
#define MIME_MAGIC_PRIV_HH

#include <stdint.h>

//======================================================================

enum OffsetType
{
    OffFlags_byte       = 0,
    OffFlags_leshort    = 1,
    OffFlags_lelong     = 2,
    OffFlags_beshort    = 3,
    OffFlags_belong     = 4
};


/*  The limited set of formats we recognise don't test for dates.
    They obviously don't determine the MIME type.  Neither are
    floating point tests used.
*/
enum TestType
{
    TestType_end        = 0,    // this marks the last entry

    TestType_byte       = 1,    
    TestType_default    = 2,    

    TestType_beshort    = 3,    // big-endian short
    TestType_leshort    = 4,    // little-endian short

    TestType_belong     = 5,    // big-endian long 
    TestType_lelong     = 6,    // little-endian long 

    TestType_bequad     = 7,    // big-endian 64 bits 
    TestType_lequad     = 8,    // little-endian 64 bits 

    TestType_regex      = 9,    // REVISIT try to avoid this
    TestType_search     = 10,
    TestType_string     = 11
};


/*  This library doesn't distinguish between text and binary data.
*/
enum StringFlags
{
    StrFlag_CompactWS  = 1 << 0, // Require WS and elide it
    StrFlag_IgnoreWS   = 1 << 1, // Ignore WS
    StrFlag_MatchLower = 1 << 2, // match lower-case in the string without case
    StrFlag_MatchUpper = 1 << 3, // match upper-case in the string without case
    StrFlag_Trim       = 1 << 4, // trim WS before matching
};


/*  This is applied before an entry is matched.
*/
enum StrengthOper
{
    Strength_None   = 0,
    Strength_Add    = 1,
    Strength_Sub    = 2,
    Strength_Mul    = 3,
    Strength_Div    = 4
};


/*  This represents one row of the magic file in an easy to digest
    format.
*/
typedef struct Entry
{
    uint8_t     newSection : 1;     // This entry starts a new section
    uint8_t     indirect : 1;       // offset points to where to read the offset
    uint8_t     unsignedType : 1;   // numeric tests are unsigned
    uint8_t     strengthOper : 3;
    uint8_t     offsetType : 3;     // from OffsetType

    uint8_t     level;
    uint8_t     testType;       // from TestType

    uint8_t     stringFlags;    // from StringFlags
    uint8_t     strengthValue;

    uint16_t    offset;         // the first offset into the file
    uint16_t    extraOffset;    // added after indirection

    uint32_t    testMask;       // use if non-zero

    const char* test;           // the string to match against
    const char* mime;           // the MIME type to declare if matched

} Entry;



//======================================================================

#endif // MIME_MAGIC_PRIV_HH
