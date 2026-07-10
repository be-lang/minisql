import random, sys
random.seed(42)
cities = ["Zurich","Bern","Geneva","Basel","Lausanne","Lugano"]
N = int(sys.argv[1]); M = int(sys.argv[2]); out = sys.argv[3]
with open(f"{out}/customers.csv","w") as f:
    f.write("id,name,city\n")
    for i in range(1, N+1):
        f.write(f"{i},Customer{i},{cities[i % len(cities)]}\n")
with open(f"{out}/orders.csv","w") as f:
    f.write("order_id,customer_id,amount\n")
    for j in range(1, M+1):
        f.write(f"{j},{random.randint(1,N)},{random.randint(1,1000)}\n")
print(f"wrote {N} customers, {M} orders to {out}/")
