i = 0
j = 0

vector1 = [1, 2, 3, 4, 5]
vector2 = [1, 3, 4, 6, 7]

vector1 = [1, 20, 3]
vector2 = [1, 2, 3, 4, 5, 6]

additions = []
deletions = []

while i < len(vector1) or j < len(vector2):
    if i >= len(vector1):
        additions.append(vector2[j])
        j += 1

    elif j >= len(vector2):
        deletions.append(vector1[i])
        i += 1
    
    elif vector1[i] == vector2[j]:
        j += 1
        i += 1
    
    elif vector1[i] < vector2[j]:
        deletions.append(vector1[i])
        i += 1

    else:
        additions.append(vector2[j])
        j += 1

print(additions)
print(deletions)
    