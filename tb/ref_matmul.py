import numpy as np
import sys

def generate(n=4, seed=42):
    rng = np.random.RandomState(seed)
    A = rng.randint(-128, 127, (n, n)).astype(np.int16)
    B = rng.randint(-128, 127, (n, n)).astype(np.int16)
    C = A.astype(np.int32) @ B.astype(np.int32)

    with open("tb/test_vectors.txt", "w") as f:
        f.write(f"{n}\n")
        for row in A:
            f.write(" ".join(str(x) for x in row) + "\n")
        for row in B:
            f.write(" ".join(str(x) for x in row) + "\n")
        for row in C:
            f.write(" ".join(str(x) for x in row) + "\n")

if __name__ == "__main__":
    generate(int(sys.argv[1]) if len(sys.argv) > 1 else 4,
             int(sys.argv[2]) if len(sys.argv) > 2 else 42)
