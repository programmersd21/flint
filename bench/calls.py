def noop(x):
    return x

acc = 0
i = 0
while i < 3000000:
    acc = noop(acc + 1)
    i += 1
print(acc)
