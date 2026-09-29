parts = []
i = 0
while i < 200000:
    parts.append("item" + str(i))
    i += 1
line = ",".join(parts)

print(len(line.split(",")))
print("item42" in line)
print(line.startswith("item0,item1"))

print(len(line.replace("item", "ITEM")))
