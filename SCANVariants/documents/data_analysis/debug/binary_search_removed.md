int size:4
n:124836180, m:3612134270

Total input cost:107245 ms
1st: prune execution time:21014 ms
2nd: check core first-phase bsp time:490334 ms
2nd: check core second-phase bsp time:776 ms
3rd: core clustering time:544 ms
4th: non-core clustering time:8450 ms
Total time without IO:521424 ms
Total output cost:1974 ms

// failure

int size:4
n:124836180, m:3612134270

Total input cost:73673 ms
1st: prune execution time:21002 ms
2nd: check core first-phase bsp time:490676 ms
err:1612104523 1612104523 52665228

// debug info

i:38964727  1612104427,1612104540
i:21789429  958735381,958735458
err:1612104523 1612104523 52665228

bug fix

fix overflow bug

```cpp
auto mid = static_cast<ui>((static_cast<unsigned long>(offset_beg) + offset_end) / 2);
```

binary search detail

``cpp
ui Graph::BinarySearch(vector<int> &array, ui offset_beg, ui offset_end, int val) {
    auto mid = static_cast<ui>((static_cast<unsigned long>(offset_beg) + offset_end) / 2);
    if (offset_beg >= offset_end) {
        for (auto i = 0; i < n; i++) {
            if (out_edge_start[i] <= offset_beg && out_edge_start[i + 1] > offset_beg) {
                cout << "i:" << i << out_edge_start[i] << "," << out_edge_start[i + 1] << endl;
            }
        }
        cout << "err:" << offset_beg << " " << offset_end << " " << val << endl;
        exit(1);
    }
    if (array[mid] == val) { return mid; }
    return val < array[mid] ? BinarySearch(array, offset_beg, mid, val) : BinarySearch(array, mid + 1, offset_end, val);
}
```

remove debugging info

```cpp
//    if (offset_beg >= offset_end) {
//        for (auto i = 0; i < n; i++) {
//            if (out_edge_start[i] <= offset_beg && out_edge_start[i + 1] > offset_beg) {
//                cout << "i:" << i << out_edge_start[i] << "," << out_edge_start[i + 1] << endl;
//            }
//        }
//        cout << "err:" << offset_beg << " " << offset_end << " " << val << endl;
//        exit(1);
//    }
```