struct link { struct link *next; int v; };
struct pair { struct pair *other; struct link *head; };
