const app = require('express')();
const port = 3000;

app.listen(port, () => {
  console.log(`Server is running on http://localhost:${port}`);
});

app.get('/', (req, res) => {
  res.send('Hello, World!');
});


app.get('/about', (req, res) => {
  res.status(200).send({
    message: 'This is the about page.',
    status: 'success'
  });
});