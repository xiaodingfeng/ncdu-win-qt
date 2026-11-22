export interface SaveFolder {
  id: string;
  nameKey: string;
  folder: string;
  path: string;
  defaultPath: string;
}

export interface SavePair {
  from: string;
  to: string;
}

export interface SaveMoveProgress {
  copied: number;
  total: number;
  path: string;
}

export interface SaveLeftover {
  from: string;
  to: string;
}

export interface SaveMoveResult {
  movedBytes: number;
  leftovers: SaveLeftover[];
}
