xs = []
i = 0
while i < 1000000:
    xs.append(i * 3)
    i += 1
acc = 0
j = 0
while j < 1000000:
    acc += xs[j]
    j += 1
print(acc)
