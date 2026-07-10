import random,sys
random.seed(7)
N=20000; M=100000
regions=["North","South","East","West","Central"]
with open("bench/regions.csv","w") as f:
    f.write("region_id,region_name\n")
    for i,r in enumerate(regions,1): f.write(f"{i},{r}\n")
with open("bench/cust3.csv","w") as f:
    f.write("id,name,region_id\n")
    for i in range(1,N+1): f.write(f"{i},Customer{i},{random.randint(1,len(regions))}\n")
with open("bench/ord3.csv","w") as f:
    f.write("order_id,customer_id,amount\n")
    for j in range(1,M+1): f.write(f"{j},{random.randint(1,N)},{random.randint(1,1000)}\n")
print("wrote regions(5), cust3(20000), ord3(100000)")
