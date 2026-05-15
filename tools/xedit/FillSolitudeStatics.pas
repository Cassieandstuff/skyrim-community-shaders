{
  FillSolitudeStatics.pas
  xEdit script to place a large number of static mesh references in and around
  Solitude for testing mesh combining. Biases toward reusing the same few base
  objects so many references share identical shader properties / materials.

  Usage: Apply to Skyrim.esm (or any loaded plugin) in xEdit.
  Creates a new ESP with placed references.
}
unit FillSolitudeStatics;

const
  // Tamriel worldspace
  kWorldspaceTamriel = $0000003C;

  // Common statics that share materials (vanilla Skyrim.esm FormIDs)
  // Barrels - all use the same barrel texture set / shader property
  kBarrelFood01       = $0001F259;  // FarmBarrel01
  kBarrelFood02       = $000BBD26;  // BarrelFood01

  // Crates - shared wood crate material
  kCrate01            = $000B6295;  // CommonCrate01
  kCrate02            = $000B6296;  // CommonCrate02

  // Sacks - shared fabric material
  kSack01             = $0009B94A;  // Sack01A
  kSack02             = $0009B94B;  // Sack01B

  // Rocks - shared rock shader
  kRock01             = $000AB3E2;  // TundraRockPile01
  kRock02             = $000AB3E3;  // TundraRockPile02

  // Wood piles - shared wood material
  kWoodPile01         = $0003AD5E;  // WoodPile01
  kWoodPile02         = $0003AD5F;  // WoodPile02

  // How many refs to place per base object type
  kRefsPerType        = 60;

  // Solitude approximate world position (Skyrim units)
  // Solitude is roughly at grid (26, 30) => world X ~67000, Y ~77000
  kBaseX = 67000.0;
  kBaseY = 77000.0;
  kBaseZ = -3800.0;

  // Spread radius (Skyrim units) - spread them around the area
  kSpreadX = 8000.0;
  kSpreadY = 8000.0;
  kSpreadZ = 200.0;

  // Grid cell size
  kCellSize = 4096;

var
  outputPlugin: IInterface;
  tamrielWorld: IInterface;
  placedCount: Integer;

function Initialize: Integer;
begin
  Result := 0;
  placedCount := 0;

  // Create output plugin
  outputPlugin := AddNewFile;
  if not Assigned(outputPlugin) then begin
    AddMessage('ERROR: No output plugin created. Aborting.');
    Result := 1;
    Exit;
  end;

  // Add Skyrim.esm as master
  AddMasterIfMissing(outputPlugin, 'Skyrim.esm');

  AddMessage('=== FillSolitudeStatics ===');
  AddMessage('Output plugin: ' + GetFileName(outputPlugin));

  // Find Tamriel worldspace
  tamrielWorld := RecordByFormID(FileByIndex(0), kWorldspaceTamriel, True);
  if not Assigned(tamrielWorld) then begin
    AddMessage('ERROR: Could not find Tamriel worldspace!');
    Result := 1;
    Exit;
  end;
  AddMessage('Found Tamriel worldspace: ' + Name(tamrielWorld));

  // Place groups of statics
  PlaceGroup(kBarrelFood01, 'BarrelGroup1', 0.0, 0.0);
  PlaceGroup(kBarrelFood02, 'BarrelGroup2', 400.0, 0.0);
  PlaceGroup(kCrate01,      'CrateGroup1',  0.0, 400.0);
  PlaceGroup(kCrate02,      'CrateGroup2',  400.0, 400.0);
  PlaceGroup(kSack01,       'SackGroup1',   800.0, 0.0);
  PlaceGroup(kSack02,       'SackGroup2',   800.0, 400.0);
  PlaceGroup(kRock01,       'RockGroup1',   0.0, 800.0);
  PlaceGroup(kRock02,       'RockGroup2',   400.0, 800.0);
  PlaceGroup(kWoodPile01,   'WoodGroup1',   1200.0, 0.0);
  PlaceGroup(kWoodPile02,   'WoodGroup2',   1200.0, 400.0);

  AddMessage(Format('Done! Placed %d references total.', [placedCount]));
end;

procedure PlaceGroup(baseFormID: Cardinal; groupName: string; offsetX, offsetY: Float);
var
  i: Integer;
  x, y, z: Float;
  angle: Float;
  radius: Float;
begin
  AddMessage(Format('Placing %d refs for %s (base %s)',
    [kRefsPerType, groupName, IntToHex(baseFormID, 8)]));

  for i := 0 to kRefsPerType - 1 do begin
    // Arrange in a grid pattern with some randomization
    // Each group gets its own cluster offset so same-material refs are near each other
    angle := (i / kRefsPerType) * 2.0 * 3.14159;
    radius := 100.0 + (i mod 10) * 80.0;

    x := kBaseX + offsetX + Cos(angle) * radius + (i mod 7) * 120.0;
    y := kBaseY + offsetY + Sin(angle) * radius + (i div 7) * 120.0;
    z := kBaseZ + (Random * kSpreadZ) - (kSpreadZ / 2.0);

    PlaceReference(baseFormID, x, y, z);
  end;
end;

procedure PlaceReference(baseFormID: Cardinal; x, y, z: Float);
var
  baseRec, cellGroup, cellBlock, cellSubBlock, cell, refGroup, newRef: IInterface;
  gridX, gridY: Integer;
begin
  // Resolve the base static record from Skyrim.esm
  baseRec := RecordByFormID(FileByIndex(0), baseFormID, True);
  if not Assigned(baseRec) then begin
    AddMessage(Format('WARNING: Could not find base record %s', [IntToHex(baseFormID, 8)]));
    Exit;
  end;

  // Calculate grid cell coordinates
  gridX := Floor(x / kCellSize);
  gridY := Floor(y / kCellSize);

  // Find or create the exterior cell
  cell := FindOrCreateExteriorCell(gridX, gridY);
  if not Assigned(cell) then begin
    AddMessage(Format('WARNING: Could not find/create cell at grid (%d, %d)', [gridX, gridY]));
    Exit;
  end;

  // Get or create the temporary references group (group type 8)
  refGroup := FindChildGroup(cell, 8, cell);
  if not Assigned(refGroup) then
    refGroup := Add(cell, 'Child Group', True);

  // Create new REFR record
  newRef := Add(refGroup, 'REFR', True);
  if not Assigned(newRef) then begin
    AddMessage('WARNING: Could not create REFR record');
    Exit;
  end;

  // Set the base object (NAME)
  Add(newRef, 'NAME', True);
  SetElementNativeValues(newRef, 'NAME', GetLoadOrderFormID(baseRec));

  // Set position (DATA)
  Add(newRef, 'DATA', True);
  SetElementNativeValues(newRef, 'DATA\Position\X', x);
  SetElementNativeValues(newRef, 'DATA\Position\Y', y);
  SetElementNativeValues(newRef, 'DATA\Position\Z', z);
  SetElementNativeValues(newRef, 'DATA\Rotation\X', 0.0);
  SetElementNativeValues(newRef, 'DATA\Rotation\Y', 0.0);
  SetElementNativeValues(newRef, 'DATA\Rotation\Z', Random * 6.28318);

  placedCount := placedCount + 1;
end;

function FindOrCreateExteriorCell(gridX, gridY: Integer): IInterface;
var
  wrldCopy, cellGroup, block, subBlock, cell: IInterface;
  blockX, blockY, subBlockX, subBlockY: Integer;
  i, j, k: Integer;
begin
  Result := nil;

  // Copy worldspace to output plugin if not already there
  wrldCopy := FindRecordInFile(outputPlugin, 'WRLD', kWorldspaceTamriel);
  if not Assigned(wrldCopy) then begin
    wrldCopy := wbCopyElementToFile(tamrielWorld, outputPlugin, False, True);
    if not Assigned(wrldCopy) then begin
      AddMessage('ERROR: Could not copy worldspace to output plugin');
      Exit;
    end;
  end;

  // Calculate block and subblock indices
  // Blocks are groups of 32x32 cells, subblocks are 8x8
  blockX := gridX div 32;
  blockY := gridY div 32;
  subBlockX := gridX div 8;
  subBlockY := gridY div 8;

  // Navigate the worldspace cell structure
  cellGroup := ChildGroup(wrldCopy);
  if not Assigned(cellGroup) then begin
    AddMessage('ERROR: No child group for worldspace');
    Exit;
  end;

  // Find or iterate exterior cell blocks
  for i := 0 to Pred(ElementCount(cellGroup)) do begin
    block := ElementByIndex(cellGroup, i);
    if GroupType(block) = 4 then begin  // Exterior Cell Block
      for j := 0 to Pred(ElementCount(block)) do begin
        subBlock := ElementByIndex(block, j);
        if GroupType(subBlock) = 5 then begin  // Exterior Cell Sub-Block
          for k := 0 to Pred(ElementCount(subBlock)) do begin
            cell := ElementByIndex(subBlock, k);
            if Signature(cell) = 'CELL' then begin
              if (GetElementNativeValues(cell, 'XCLC\X') = gridX) and
                 (GetElementNativeValues(cell, 'XCLC\Y') = gridY) then begin
                Result := cell;
                Exit;
              end;
            end;
          end;
        end;
      end;
    end;
  end;

  // Cell not found in output - we need to find and copy it from the source
  Result := FindExteriorCellInSource(gridX, gridY);
  if Assigned(Result) then begin
    Result := wbCopyElementToFile(Result, outputPlugin, False, True);
  end;
end;

function FindExteriorCellInSource(gridX, gridY: Integer): IInterface;
var
  wrldGroup, cellGroup, block, subBlock, cell: IInterface;
  i, j, k: Integer;
begin
  Result := nil;

  cellGroup := ChildGroup(tamrielWorld);
  if not Assigned(cellGroup) then Exit;

  for i := 0 to Pred(ElementCount(cellGroup)) do begin
    block := ElementByIndex(cellGroup, i);
    if GroupType(block) = 4 then begin
      for j := 0 to Pred(ElementCount(block)) do begin
        subBlock := ElementByIndex(block, j);
        if GroupType(subBlock) = 5 then begin
          for k := 0 to Pred(ElementCount(subBlock)) do begin
            cell := ElementByIndex(subBlock, k);
            if Signature(cell) = 'CELL' then begin
              if (GetElementNativeValues(cell, 'XCLC\X') = gridX) and
                 (GetElementNativeValues(cell, 'XCLC\Y') = gridY) then begin
                Result := cell;
                Exit;
              end;
            end;
          end;
        end;
      end;
    end;
  end;
end;

function FindRecordInFile(f: IInterface; sig: string; formID: Cardinal): IInterface;
var
  g, rec: IInterface;
  i: Integer;
begin
  Result := nil;
  g := GroupBySignature(f, sig);
  if not Assigned(g) then Exit;
  for i := 0 to Pred(ElementCount(g)) do begin
    rec := ElementByIndex(g, i);
    if GetLoadOrderFormID(rec) = formID then begin
      Result := rec;
      Exit;
    end;
  end;
end;

function Finalize: Integer;
begin
  Result := 0;
  if placedCount > 0 then
    AddMessage(Format('Script complete. %d references placed. Save the plugin in xEdit.', [placedCount]));
end;

end.
