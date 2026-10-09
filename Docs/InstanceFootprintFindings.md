# Instance footprint findings — CL-127004

Verified in the small two-Smelter save:

| Building PID | Observed foundation arrangement | Linked foundation entities | Reported payload/default dimensions |
| --- | --- | --- | --- |
| 356 | 4 × 3 | 12 | 4 × 3 |
| 406 | 4 × 4 | 16 | 4 × 3 |

Both buildings use the same `BD_Smelter` placement definition and `DA_Smelter`
configuration. Their payload/default `TilesData` dimensions therefore do not
uniquely describe the selected footprint of every placed instance.

The removed BuildingFootprintProbe reported PID 406 as `InstancePayload`, 4 × 3,
while its `originLattice` correctly reported 4 × 4 from linked foundation
positions. That result label represented definition-derived dimensions, not a
verified instance footprint. Foundation origins were observed geometry; they
were not proof that the probe had resolved full foundation tile extents or a
universal placement rectangle.

Foundation relationships and spatial positions are promising instance-specific
evidence. Generalization remains unproven for other building families, shared or
reused foundations, missing/incomplete foundation relationships, and instances
without an actor representation. These observations do not establish a universal
footprint resolver or justify hardcoded building sizes.

The probe was removed because each five-second sample performed 15 complete
global UObject scans. This was an unacceptable diagnostic cost; it was not
conclusively established as the sole cause of the reported gameplay freezes.
