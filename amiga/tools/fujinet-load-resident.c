#include <dos/dos.h>
#include <exec/execbase.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/resident.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include <stdio.h>
#include <string.h>

static int resident_name_matches(const struct Resident *resident,
                                 const UBYTE *first_hunk_data,
                                 const UBYTE *first_hunk_end,
                                 const char *expected_name)
{
    const UBYTE *name = (const UBYTE *)resident->rt_Name;
    const UBYTE *cursor;

    if (name < first_hunk_data || name >= first_hunk_end)
        return 0;
    for (cursor = name; cursor < first_hunk_end; ++cursor) {
        if (*cursor == '\0')
            return expected_name == NULL ||
                   strcmp((const char *)name, expected_name) == 0;
    }
    return 0;
}

static struct Resident *find_resident_tag(BPTR segment_list,
                                          const char *expected_name,
                                          ULONG *first_hunk_bytes,
                                          ULONG *resident_offset)
{
    UBYTE *segment = (UBYTE *)BADDR(segment_list);
    UBYTE *allocation;
    ULONG allocation_size;
    UBYTE *cursor;
    UBYTE *end;

    if (segment == NULL) return NULL;
    allocation = segment - sizeof(ULONG);
    allocation_size = *(ULONG *)allocation;
    if (allocation_size < sizeof(ULONG) * 2 + sizeof(struct Resident))
        return NULL;

    /* The allocation starts one ULONG before the BPTR target. The first
     * ULONG at the target links to the next segment; executable data follows.
     * Exec requires a disk resident's ROMTag to be in the first code hunk. */
    cursor = segment + sizeof(BPTR);
    end = allocation + allocation_size;
    while (cursor + sizeof(struct Resident) <= end) {
        struct Resident *resident = (struct Resident *)cursor;
        if (resident->rt_MatchWord == RTC_MATCHWORD &&
            resident->rt_MatchTag == resident &&
            (UBYTE *)resident->rt_EndSkip > cursor &&
            (UBYTE *)resident->rt_EndSkip <= end &&
            resident_name_matches(resident, segment + sizeof(BPTR), end,
                                  expected_name)) {
            if (first_hunk_bytes != NULL)
                *first_hunk_bytes = allocation_size;
            if (resident_offset != NULL)
                *resident_offset = (ULONG)(cursor - segment);
            return resident;
        }
        cursor += sizeof(UWORD);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const char *module;
    const char *resident_name;
    BPTR segment_list;
    struct Resident *resident;
    APTR initialized;
    ULONG first_hunk_bytes = 0;
    ULONG resident_offset = 0;

    if (argc < 2 || argc > 3) {
        fprintf(stderr,
                "Usage: fujinet-load-resident MODULE [RESIDENT-NAME]\n");
        return RETURN_ERROR;
    }
    module = argv[1];
    resident_name = argc == 3 ? argv[2] : NULL;

    if (resident_name != NULL &&
        FindName(&SysBase->DeviceList, (CONST_STRPTR)resident_name) != NULL) {
        printf("Resident device already loaded: %s\n", resident_name);
        return RETURN_OK;
    }

    segment_list = LoadSeg((CONST_STRPTR)module);
    if (segment_list == 0) {
        fprintf(stderr, "Cannot load resident module %s (IoErr=%ld)\n",
                module, (long)IoErr());
        return RETURN_FAIL;
    }

    resident = find_resident_tag(segment_list, resident_name,
                                 &first_hunk_bytes, &resident_offset);
    if (resident == NULL) {
        fprintf(stderr, "No matching resident tag in %s\n", module);
        UnLoadSeg(segment_list);
        printf("Resident segment released after scan failure\n");
        return RETURN_FAIL;
    }
    if (resident->rt_Type != NT_DEVICE) {
        fprintf(stderr, "Unsupported resident type %u in %s\n",
                (unsigned)resident->rt_Type, module);
        UnLoadSeg(segment_list);
        printf("Resident segment released after type failure\n");
        return RETURN_FAIL;
    }

    initialized = InitResident(resident, (ULONG)segment_list);
    if (initialized == NULL) {
        fprintf(stderr, "Resident initialization failed: %s\n",
                resident->rt_Name);
        UnLoadSeg(segment_list);
        printf("Resident segment released after initialization failure\n");
        return RETURN_FAIL;
    }

    /* The initialized device/library executes from this segment and receives
     * its BPTR for eventual expunge. It must remain allocated on success. */
    printf("Resident loaded: %s first_hunk_bytes=%lu resident_offset=%lu\n",
           resident->rt_Name, (unsigned long)first_hunk_bytes,
           (unsigned long)resident_offset);
    return RETURN_OK;
}
