# Delta Image Archive (`.dia`)

**An optimized file format for storing large collections of similar images.**

This project introduces the Delta Image Archive (`.dia`), a specialized archive format designed to drastically reduce the storage size of image sequences with high similarity. It works by intelligently finding the most similar base for each frame, creating a dependency tree, and then storing only the unique "delta" pixels for each dependent image. The optimized delta images and a reconstruction map are then packaged into a single `.dia` file.

## Why?

I have a few hundred thousand images archived on my HDD. On average, I can save about 25% on each PNG by compressing with `oxipng`, and converting that into a lossless JXL saves an extra 25%.

While this is nice, many of them are slight variations on another image: different eye/hair color, different facial/hand expression, added or removed text, etc. Storing the entire image for every minor variation is highly inefficient.

As an example, these 8 images were originally **43.3 MB** in total. (art by [Reona KFC](https://www.pixiv.net/en/users/1627777) )
<img width="1536" height="432" alt="Grid with 8 images with small variace" src="https://github.com/user-attachments/assets/02ae5df6-b22a-4d39-bed7-5fbb648d1cee" />

- After optimizing with `oxipng`, their total size is **33.1 MB**.
- After converting the optimized PNGs to lossless JXL, the size is **19.8 MB**.

That's not bad, but it could be much better. By comparing the images and storing only the unique pixels for each variation, we get this:
<img width="3072" height="432" alt="Same 8 images next to the uniqe pixels in that image" src="https://github.com/user-attachments/assets/e249c0fd-2aeb-4312-bfc7-759f12156544" />

The total size of the `.dia` archive for these images is **7.6 MB** (using delta PNGs) or just **4.4 MB** if those deltas are then converted to JXL.

### Results
<img width="1395" height="632" alt=" Graph comparing the size changes" src="https://github.com/user-attachments/assets/d2ccae8c-666c-43cb-a7bc-52c00fb97a31" />

As basic and flawed as this may be, reducing the storage from **43.3 MB** (original) or **19.8 MB** (JXL) down to **4.4 MB** is a significant improvement.

## How It Works

The magic is in the two-phase packing process, which builds an optimal dependency graph to ensure maximum pixel reuse.

1.  **Phase 1: Analysis & Graph Building**
    *   **Recursive Scan:** The packer recursively finds all images in the target directory.
    *   **All-Pairs Scoring:** It calculates a "similarity score" (number of identical pixels) for every possible pair of images. This is the most computationally intensive step.
    *   **Maximum Spanning Forest:** Using the similarity scores as edge weights, the algorithm builds a [Maximum Spanning Forest](httpss://en.wikipedia.org/wiki/Maximum_spanning_tree). This connects all images into one or more dependency trees using the highest-scoring pairs, crucially **without creating cycles**.
    *   **Optimal Root Selection:** For each tree in the forest, the image with the *smallest original file size* is chosen as the "root." This image will be stored in full. This minimizes the baseline size of the archive.

2.  **Phase 2: Image Processing & Packaging**
    *   **Delta Generation:** For every non-root image, a "delta" is created by taking the difference between it and its parent in the dependency tree. Unchanged pixels are made transparent.
    *   **Optimization:** These new delta PNGs are optimized using `oxipng` for the smallest possible file size.
    *   **Packaging:** The full-size root images, the optimized delta images, and a JSON map describing the dependency tree are all packaged into a single `.zip` archive with a `.dia` extension.
