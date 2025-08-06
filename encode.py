#!/usr/bin/env python3

import gc
import os
import shutil
import argparse
import json
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed
from itertools import combinations
import collections
import oxipng
import sys
import tempfile
import zipfile

from PIL import Image, ImageChops
import numpy as np

IMAGE_EXTENSIONS = {'.png'}

class DisjointSetUnion:
    """A simple Disjoint Set Union (DSU) or Union-Find data structure."""
    def __init__(self, items):
        self.parent = {item: item for item in items}

    def find(self, item):
        if self.parent[item] == item:
            return item
        self.parent[item] = self.find(self.parent[item])
        return self.parent[item]

    def union(self, item1, item2):
        root1 = self.find(item1)
        root2 = self.find(item2)
        if root1 != root2:
            self.parent[root2] = root1
            return True
        return False

def print_progress_bar(iteration, total, prefix='', suffix='', length=50, fill='█'):
    if total == 0: total = 1
    percent = f"{100 * (iteration / float(total)):.1f}"
    filled_length = int(length * iteration // total)
    bar = fill * filled_length + '-' * (length - filled_length)
    print(f'\r{prefix} |{bar}| {percent}% {suffix}', end='\r')
    if iteration == total:
        print()

def calculate_similarity_score(img_path1, img_path2):
    try:
        with Image.open(img_path1) as img1, Image.open(img_path2) as img2:
            if img1.size != img2.size or img1.mode != img2.mode:
                img2 = img2.resize(img1.size).convert(img1.mode)
            
            diff = ImageChops.difference(img1, img2)
            diff_arr = np.array(diff.convert('L'))
            non_zero_pixels = np.count_nonzero(diff_arr)
            total_pixels = diff_arr.shape[0] * diff_arr.shape[1]
            return total_pixels - non_zero_pixels
    except Exception:
        return -1

def process_image(current_img_path, base_img_path, output_path):
    try:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        with Image.open(current_img_path) as img_current, Image.open(base_img_path) as img_base:
            img_current_rgb = img_current.convert('RGB')
            del img_current
            img_base_rgb = img_base.convert('RGB')
            del img_base
            diff = ImageChops.difference(img_current_rgb, img_base_rgb)
            del img_base_rgb
            mask = diff.convert('L').point(lambda p: 255 if p > 0 else 0)
            del diff
            sanitized_image = img_current_rgb.convert('RGBA')
            del img_current_rgb
            sanitized_image.putalpha(mask)
            del mask
            rgba_array = np.array(sanitized_image, dtype=np.uint8)
            height, width, channels = rgba_array.shape
            del sanitized_image
            data = rgba_array.tobytes()
            del rgba_array
            color_type = oxipng.ColorType.rgba()  
            raw = oxipng.RawImage(data, width, height, color_type=color_type)
            del data, color_type
            optimized = raw.create_optimized_png(level=6, optimize_alpha=True)
            del raw
            with open(output_path, "wb") as f:
              f.write(optimized)
            del optimized
        gc.collect()
        return True
    except Exception as e:
        gc.collect()
        exc_type, exc_obj, exc_tb = sys.exc_info()
        fname = os.path.split(exc_tb.tb_frame.f_code.co_filename)[1]
        print(f"\nException: {exc_type} in {fname} at line {exc_tb.tb_lineno}")
        print(f"Error processing {os.path.basename(str(current_img_path))}: {e}")
        return False

def main():
    parser = argparse.ArgumentParser(
        description="Recursively finds, optimizes, and zips an image sequence.",
        formatter_class=argparse.RawTextHelpFormatter
    )
    parser.add_argument("input_dir", help="Directory containing source images and subdirectories.")
    parser.add_argument("-w", "--workers", type=int, default=os.cpu_count() // 2, help="Number of concurrent threads.")
    args = parser.parse_args()

    input_dir = Path(args.input_dir).resolve()
    output_zip_path = Path(f"{input_dir}.dia")

    if not input_dir.is_dir():
        print(f"Error: Input directory not found at '{input_dir}'")
        return

    with tempfile.TemporaryDirectory() as temp_dir:
        output_dir = Path(temp_dir)

        print("Scanning for images recursively...")
        all_image_paths_abs = [p for p in input_dir.rglob('**/*') if p.is_file() and p.suffix.lower() in IMAGE_EXTENSIONS]
        image_paths_rel = [str(p.relative_to(input_dir)) for p in all_image_paths_abs]
        try:
            image_paths_rel.sort(key=lambda f: int(Path(f).stem))
        except (ValueError, IndexError):
            image_paths_rel.sort()
        
        if len(image_paths_rel) < 2:
            print("At least two images are required for optimization.")
            return

        path_to_id = {path: str(i) for i, path in enumerate(image_paths_rel)}
        id_to_path = {str(i): path for i, path in enumerate(image_paths_rel)}

        print(f"Found {len(image_paths_rel)} images. Starting Phase 1: Scoring all pairs...")
        all_pairs = list(combinations(image_paths_rel, 2))
        all_scores = []
        with ThreadPoolExecutor(max_workers=args.workers) as executor:
            future_to_pair = {executor.submit(calculate_similarity_score, input_dir / p[0], input_dir / p[1]): p for p in all_pairs}
            for i, future in enumerate(as_completed(future_to_pair), 1):
                pair = future_to_pair[future]
                try:
                    score = future.result()
                    if score != -1:
                        id1, id2 = path_to_id[pair[0]], path_to_id[pair[1]]
                        all_scores.append((score, id1, id2))
                except Exception as e:
                    print(f"\nError scoring pair {pair}: {e}")
                print_progress_bar(i, len(all_pairs), prefix='Phase 1/2:', suffix='Scoring Pairs')

        all_scores.sort(key=lambda x: x[0], reverse=True)
        dsu = DisjointSetUnion(id_to_path.keys())
        adjacency_list = collections.defaultdict(list)
        for score, u_id, v_id in all_scores:
            if dsu.union(u_id, v_id):
                adjacency_list[u_id].append(v_id)
                adjacency_list[v_id].append(u_id)

        root_image_ids, dependencies_by_id, visited_ids = [], {}, set()
        for image_id in id_to_path.keys():
            if image_id not in visited_ids:
                component_node_ids, q = [], collections.deque([image_id])
                component_visited_ids = {image_id}
                while q:
                    node_id = q.popleft()
                    component_node_ids.append(node_id)
                    for neighbor_id in adjacency_list[node_id]:
                        if neighbor_id not in component_visited_ids:
                            component_visited_ids.add(neighbor_id)
                            q.append(neighbor_id)
                optimal_root_id = min(component_node_ids, key=lambda nid: (input_dir / id_to_path[nid]).stat().st_size)
                root_image_ids.append(optimal_root_id)
                q = collections.deque([optimal_root_id])
                visited_ids.add(optimal_root_id)
                while q:
                    parent_id = q.popleft()
                    for child_id in adjacency_list[parent_id]:
                        if child_id not in visited_ids:
                            visited_ids.add(child_id)
                            dependencies_by_id[child_id] = parent_id
                            q.append(child_id)

        map_data = {"image_map": id_to_path, "root_images": sorted(root_image_ids, key=int), "dependencies": dependencies_by_id}
        map_file_path = output_dir / "optimization_map.json"
        with open(map_file_path, 'w', encoding='utf-8') as f:
            json.dump(map_data, f, indent=2, sort_keys=True, ensure_ascii=False)
        print(f"\nOptimization map created in temporary directory.")

        print("Starting Phase 2: Processing images...")
        for root_id in root_image_ids:
            root_path_rel = id_to_path[root_id]
            dest_path = output_dir / root_path_rel
            dest_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(input_dir / root_path_rel, dest_path)
        
        total_to_process = len(dependencies_by_id) + len(root_image_ids)
        processed_count = len(root_image_ids)
        print_progress_bar(processed_count, total_to_process, prefix='Phase 2/2:', suffix='Processing')
        with ThreadPoolExecutor(max_workers=4) as executor:
            futures = {executor.submit(process_image, input_dir / id_to_path[cid], input_dir / id_to_path[pid], output_dir / id_to_path[cid]): cid for cid, pid in dependencies_by_id.items()}
            for future in as_completed(futures):
                if future.result():
                    processed_count += 1
                    print_progress_bar(processed_count, total_to_process, prefix='Phase 2/2:', suffix='Processing')
        
        print("\nZipping output files...")
        with zipfile.ZipFile(output_zip_path, 'w', zipfile.ZIP_DEFLATED) as zipf:
            for file_path in output_dir.rglob('*'):
                if file_path.is_file():
                    arcname = file_path.relative_to(output_dir)
                    zipf.write(file_path, arcname)
    
    print(f"Optimization complete. Output saved to {output_zip_path}")

if __name__ == "__main__":
    main()
